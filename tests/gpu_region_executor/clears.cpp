// SPDX-License-Identifier: MPL-2.0
// Exact executor/render-pass bodies; deterministic allocation and invalidation seams.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>
#include <boost/container/small_vector.hpp>
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#include <vulkan/vulkan_raii.hpp>
#include <common/mapping_identity.h>
using u32 = uint32_t;
using ContextTag = uint64_t;
using RecursiveSpinLock = std::recursive_mutex;
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(T &single) : std::span<T>(&single, 1) {}
};
namespace ranges {
    using std::ranges::all_of; using std::ranges::transform;
    namespace views { template<class A, class B> auto concat(const A &a, const B &b) {
        std::vector<typename A::value_type> result(a.begin(), a.end());
        result.insert(result.end(), b.begin(), b.end()); return result;
    } }
}
struct AllocationControl { static inline bool fail{}; static inline int remaining{-1}; };
template<class T> struct NodeAllocator {
    using value_type = T;
    NodeAllocator() = default;
    template<class U> NodeAllocator(const NodeAllocator<U> &) {}
    T *allocate(size_t count) {
        if (AllocationControl::fail || AllocationControl::remaining == 0) throw std::bad_alloc{};
        if (AllocationControl::remaining > 0) --AllocationControl::remaining;
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T *pointer, size_t count) { std::allocator<T>{}.deallocate(pointer, count); }
    template<class U> bool operator==(const NodeAllocator<U> &) const { return true; }
};
namespace skyline::gpu {
    struct Texture;
    struct Buffer;
    struct TextureView;
    struct UsageTracker {};
    struct GPU { struct { bool supportsImagelessFramebuffers{}; } traits; };
    struct FenceCycle {};
    struct BufferBinding {};
    struct TextureAliasReconciliationRequest {
        std::shared_ptr<Texture> target;
        std::shared_ptr<Buffer> bufferTarget;
        TextureAliasReconciliationRequest(std::shared_ptr<Texture> target = {}, std::vector<int> = {}) : target(std::move(target)) {}
    };
    namespace texture {
        enum class RenderPassUsage { None, Sampled, RenderTarget };
        struct Region { uint64_t hostVersion{1}; };
        struct RegionAuthority { std::recursive_mutex mutex; std::vector<Region> regions{{}}; };
        struct Binding { size_t region{}; uint64_t residentVersion{1}; };
    }
    struct Buffer : std::enable_shared_from_this<Buffer> {
        std::atomic<bool> regionTracked{}, regionStale{};
        std::atomic<std::shared_ptr<texture::RegionAuthority>> regionAuthority;
        std::vector<texture::Binding> regionBindings;
        RecursiveSpinLock stateMutex;
        enum class DirtyState { Clean, CpuDirty } dirtyState{DirtyState::Clean};
        ContextTag pendingRegionUseTag{};
        static void ValidateRegionWrites(span<struct BufferView>, span<TextureView *>) {}
        void CommitGpuWrite(UsageTracker &, vk::DeviceSize, vk::DeviceSize) {}
    };
    struct BufferView {
        Buffer *buffer{};
        explicit operator bool() const { return buffer; }
        Buffer *GetBuffer() const { return buffer; }
        vk::DeviceSize size{};
        vk::DeviceSize GetOffset() const { return 0; }
    };
    struct Texture : std::enable_shared_from_this<Texture> {
        std::atomic<bool> regionTracked{true}, regionStale{};
        std::atomic<std::shared_ptr<texture::RegionAuthority>> regionAuthority{std::make_shared<texture::RegionAuthority>()};
        std::vector<texture::Binding> regionBindings{{}};
        std::recursive_mutex stateMutex;
        enum class DirtyState { Clean, CpuDirty } dirtyState{DirtyState::Clean};
        struct Dimensions : vk::Extent3D {
            Dimensions(u32 width, u32 height, u32 depth) : vk::Extent3D{width, height, depth} {}
            operator vk::Extent2D() const { return {width, height}; }
        } dimensions{64, 32, 1};
        u32 layerCount{1};
        vk::ImageCreateFlags flags;
        vk::ImageLayout layout{vk::ImageLayout::eGeneral};
        unsigned writes{}, preparations{};
        bool failPreparation{};
        bool ValidateRenderPassUsage(u32, texture::RenderPassUsage) { return true; }
        void UpdateRenderPassUsage(u32, texture::RenderPassUsage usage) { if (usage == texture::RenderPassUsage::RenderTarget) ++writes; }
        texture::RenderPassUsage GetLastRenderPassUsage() { return texture::RenderPassUsage::None; }
        vk::PipelineStageFlags GetReadStageMask() { return {}; }
        const std::vector<vk::Format> &GetBackingViewFormats() { static const std::vector<vk::Format> formats; return formats; }
        void MarkGpuDirty(UsageTracker &) { ++writes; }
    };
    struct Format {
        vk::Format vkFormat{vk::Format::eR8G8B8A8Unorm};
        vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor};
        operator vk::Format() const { return vkFormat; }
    };
    struct TextureView {
        std::shared_ptr<Texture> texture{std::make_shared<Texture>()};
        Format formatStorage;
        Format *format{&formatStorage};
        vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::ImageViewType type{vk::ImageViewType::e2D};
        unsigned views{};
        std::function<void(unsigned)> onView;
        vk::ImageView GetView() {
            if (onView) onView(++views);
            return vk::ImageView{reinterpret_cast<VkImageView>(this)};
        }
        vk::ImageUsageFlags GetUsage() { return vk::ImageUsageFlagBits::eColorAttachment; }
    };
}
#include "clear_nodes.h"
namespace skyline::gpu::interconnect::node {
#include "clear_nodes.inc"
}
namespace skyline::gpu::interconnect {
    struct CommandExecutor {
        MappingIdentity::Epoch textureMappingEpoch;
        boost::container::small_vector<Texture *, 8> regionOperationUses;
        boost::container::small_vector<Buffer *, 8> regionOperationBufferUses;
        boost::container::small_vector<BufferView, 8> regionOperationBufferWrites;
        struct RegionPublication {
            MappingIdentity::Guard mappings;
            boost::container::small_vector<std::unique_lock<RecursiveSpinLock>, 8> bufferStates;
            boost::container::small_vector<std::unique_lock<std::recursive_mutex>, 8> states;
            boost::container::small_vector<std::unique_lock<std::recursive_mutex>, 4> metadata;
        };
        RegionPublication AcquireRegionPublication(span<TextureView *>, span<BufferView> = {});
        struct Slot { std::list<node::NodeVariant, NodeAllocator<node::NodeVariant>> nodes, pendingPostRenderPassNodes; };
        std::unique_ptr<Slot> slot{std::make_unique<Slot>()};
        node::RenderPassNode *renderPass{};
        decltype(slot->nodes)::iterator renderPassIt;
        u32 renderPassIndex{1}, subpassCount{};
        std::vector<vk::ImageView> lastSubpassColorAttachments, lastSubpassInputAttachments;
        vk::ImageView lastSubpassDepthStencilAttachment{};
        std::vector<std::function<void()>> subpassBoundaryCallbacks;
        ContextTag tag{1};
        UsageTracker usageTracker;
        GPU gpu;
        using SubpassFunction = std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32)>;
        struct SubpassHooks { SubpassFunction before; };
        using SubpassHookFactory = std::function<SubpassHooks(u32)>;
        struct Settings { std::shared_ptr<unsigned> executorFlushThreshold{std::make_shared<unsigned>(100000)}; };
        struct { std::shared_ptr<Settings> settings{std::make_shared<Settings>()}; } state;
        void Submit() { throw std::runtime_error("Unexpected test submit"); }
        void AddSubpass(SubpassFunction &&, vk::Rect2D, span<TextureView *>, span<TextureView *>, span<TextureView *>, TextureView *, bool, vk::PipelineStageFlags, vk::PipelineStageFlags, SubpassHookFactory, span<TextureView *>, span<TextureView *>);
        void PreflightTextureUses(span<TextureView *>, span<TextureView *> writers) {
            for (auto view : writers) if (view) {
                view->GetView();
                ++view->texture->preparations;
                if (view->texture->failPreparation) throw std::bad_alloc{};
            }
        }
        bool CreateRenderPassWithSubpass(vk::Rect2D, span<TextureView *>, span<TextureView *>, span<TextureView *>, TextureView *, bool = false, vk::PipelineStageFlags = {}, vk::PipelineStageFlags = {}, span<TextureView *> = {}, bool = false);
        void AddClearSubpass(TextureView *, const vk::ClearValue &, bool);
        void AddClearColorSubpass(TextureView *, const vk::ClearColorValue &);
        void AddClearDepthStencilSubpass(TextureView *, const vk::ClearDepthStencilValue &);
    };
#include "clear_executor.inc"
}
using namespace skyline::gpu;
using interconnect::CommandExecutor;
const vk::ClearColorValue color{std::array<float, 4>{0.1f, 0.2f, 0.3f, 1.0f}};

int main() {
    for (bool depth : {false, true}) {
        TextureView view; GPU gpu;
        interconnect::node::RenderPassNode pass{vk::Rect2D{}};
        TextureView *attachment{&view};
        pass.AddSubpass({}, depth ? span<TextureView *>{} : span<TextureView *>{attachment}, depth ? &view : nullptr, gpu);
        assert(pass.clearValues.capacity() >= 1); // actual AddAttachment prepares capacity
        // Also verify direct Clear callers retain strong exception safety if
        // they have released the prepared capacity before invoking it.
        decltype(pass.clearValues) empty;
        pass.clearValues.swap(empty);
        AllocationControl::fail = true;
        bool failed{};
        try {
            if (depth) pass.ClearDepthStencilAttachment({0.5f, 7}, gpu);
            else pass.ClearColorAttachment(0, color, gpu);
        } catch (const std::bad_alloc &) { failed = true; }
        AllocationControl::fail = false;
        assert(failed && pass.clearValues.empty());
        bool optimized = depth ? pass.ClearDepthStencilAttachment({0.5f, 7}, gpu) : pass.ClearColorAttachment(0, color, gpu);
        assert(optimized && pass.clearValues.size() == 1);
        std::puts(depth ? "PASS: failed depth/stencil clear storage allocation leaves loadOp unchanged" : "PASS: failed color clear storage allocation leaves loadOp unchanged");
    }
    for (bool depth : {false, true}) {
        TextureView view;
        if (depth) view.format->vkAspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        CommandExecutor executor;
        auto clear = [&] {
            if (depth) executor.AddClearDepthStencilSubpass(&view, {0.5f, 7});
            else executor.AddClearColorSubpass(&view, color);
        };
        view.texture->failPreparation = true;
        bool failed{};
        try { clear(); } catch (const std::bad_alloc &) { failed = true; }
        assert(failed && view.texture->writes == 0 && executor.slot->nodes.empty() && !executor.renderPass);
        view.texture->failPreparation = false;
        clear();
        assert(view.texture->writes == 1 && executor.slot->nodes.size() == 1);
        assert(executor.renderPass->clearValues.size() == 1);
        if (depth) assert(executor.renderPass->clearValues[0].depthStencil == (vk::ClearDepthStencilValue{0.5f, 7}));
        else assert(executor.renderPass->clearValues[0].color.uint32 == color.uint32);
        // Identical clear retains loadOp CLEAR without a clearAttachments node.
        clear();
        assert(view.texture->writes == 2 && executor.slot->nodes.size() == 1);
        auto *oldPass = executor.renderPass;
        AllocationControl::fail = true;
        failed = false;
        try { clear(); } catch (const std::bad_alloc &) { failed = true; }
        AllocationControl::fail = false;
        assert(failed && view.texture->writes == 2 && executor.renderPass == oldPass && executor.slot->nodes.size() == 1);
        std::puts(depth ? "PASS: depth/stencil preparation failure preserves loadOp CLEAR and publishes no writer" : "PASS: color preparation failure preserves loadOp CLEAR and publishes no writer");
    }
    for (bool mapping : {false, true}) {
        TextureView view; CommandExecutor executor;
        skyline::MappingIdentity identity;
        if (mapping) executor.textureMappingEpoch = identity.Capture(0, 4096).ReadEpoch();
        // First GetView is preflight, second happens while preparing the new RP.
        // The last publication lease must reject changes after its initial lease.
        view.onView = [&](unsigned count) { if (count == (mapping ? 1u : 2u)) {
            if (mapping) { auto mutation = identity.Invalidate(0, 4096); }
            else view.texture->dirtyState = Texture::DirtyState::CpuDirty;
        } };
        bool rejected{};
        try { executor.AddClearColorSubpass(&view, color); }
        catch (const TextureAliasReconciliationRequest &) { rejected = true; }
        assert(rejected && view.texture->writes == 0 && executor.slot->nodes.empty() && !executor.renderPass);
        std::puts(mapping ? "PASS: mapping invalidation during clear preparation rejects loadOp/writer publication" : "PASS: late CPU invalidation rolls back clear preparation before loadOp/writer publication");
    }
    {
        TextureView first, second; CommandExecutor executor;
        executor.AddClearColorSubpass(&first, color);
        auto *oldPass = executor.renderPass;
        executor.slot->pendingPostRenderPassNodes.emplace_back(std::in_place_type<interconnect::node::RenderPassEndNode>);
        second.onView = [&](unsigned count) { if (count == 2) second.texture->dirtyState = Texture::DirtyState::CpuDirty; };
        bool rejected{};
        try { executor.AddClearColorSubpass(&second, color); }
        catch (const TextureAliasReconciliationRequest &) { rejected = true; }
        assert(rejected && second.texture->writes == 0 && first.texture->writes == 1);
        assert(executor.renderPass == oldPass && executor.slot->nodes.size() == 1 && executor.slot->pendingPostRenderPassNodes.size() == 1);
        assert(oldPass->clearValues[0].color.uint32 == color.uint32);
        std::puts("PASS: failed split clear restores the prior pass and pending post-pass nodes");
        second.onView = {};
        second.texture->dirtyState = Texture::DirtyState::Clean;
        // Prepared clear and end-pass nodes allocate; the new RP node fails.
        AllocationControl::remaining = 2;
        bool allocationFailed{};
        try { executor.AddClearColorSubpass(&second, color); }
        catch (const std::bad_alloc &) { allocationFailed = true; }
        AllocationControl::remaining = -1;
        assert(allocationFailed && second.texture->writes == 0 && first.texture->writes == 1);
        assert(executor.renderPass == oldPass && executor.slot->nodes.size() == 1 && executor.slot->pendingPostRenderPassNodes.size() == 1);
        std::puts("PASS: new-pass node allocation failure restores pending post-pass nodes before retry");
    }
    {
        TextureView first, second; CommandExecutor executor;
        executor.AddClearColorSubpass(&first, color);
        auto *oldPass = executor.renderPass;
        executor.subpassBoundaryCallbacks.push_back([&] {
            std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> endEarlierQuery = [](auto &, const auto &, auto &) {};
            executor.slot->nodes.emplace_back(std::in_place_type<interconnect::node::FunctionNode>, std::move(endEarlierQuery));
        });
        second.onView = [&](unsigned count) { if (count == 2) second.texture->dirtyState = Texture::DirtyState::CpuDirty; };
        bool rejected{};
        try { executor.AddClearColorSubpass(&second, color); }
        catch (const TextureAliasReconciliationRequest &) { rejected = true; }
        assert(rejected && executor.renderPass == oldPass && executor.slot->nodes.size() == 2);
        assert(std::holds_alternative<interconnect::node::FunctionNode>(executor.slot->nodes.back()));
        assert(second.texture->writes == 0 && first.texture->writes == 1);
        std::puts("PASS: clear rollback retains boundary callback commands for previously recorded queries");
    }
    {
        TextureView first, second; CommandExecutor executor;
        executor.AddClearColorSubpass(&first, color);
        auto *oldPass = executor.renderPass;
        executor.subpassBoundaryCallbacks.push_back([&] {
            std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)> endEarlierQuery = [](auto &, const auto &, auto &) {};
            executor.slot->nodes.emplace_back(std::in_place_type<interconnect::node::FunctionNode>, std::move(endEarlierQuery));
        });
        TextureView *attachment = &second;
        bool rejected{};
        try {
            executor.AddSubpass([](auto &, const auto &, auto &, auto, auto) {}, vk::Rect2D{.extent = vk::Extent2D{64, 32}}, {}, {}, span<TextureView *>{attachment}, nullptr, false, {}, {},
                [&](u32) { second.texture->dirtyState = Texture::DirtyState::CpuDirty; return CommandExecutor::SubpassHooks{}; }, {}, {});
        } catch (const TextureAliasReconciliationRequest &) { rejected = true; }
        assert(rejected && executor.renderPass == oldPass && executor.slot->nodes.size() == 2);
        assert(std::holds_alternative<interconnect::node::FunctionNode>(executor.slot->nodes.back()));
        assert(second.texture->writes == 0 && first.texture->writes == 1);
        std::puts("PASS: draw rollback also retains boundary callbacks and rejects late hook invalidation");
    }
    {
        TextureView view; CommandExecutor executor;
        executor.AddClearColorSubpass(&view, color);
        vk::ClearColorValue other{std::array<float, 4>{1.f, 0.f, 0.f, 1.f}};
        executor.AddClearColorSubpass(&view, other);
        assert(view.texture->writes == 2 && executor.slot->nodes.size() == 2);
        assert(std::holds_alternative<interconnect::node::SubpassFunctionNode>(executor.slot->nodes.back()));
        assert(executor.renderPass->clearValues[0].color.uint32 == color.uint32);
        std::puts("PASS: conflicting repeated clear retains the earlier loadOp and prepares clearAttachments fallback");
    }
}
