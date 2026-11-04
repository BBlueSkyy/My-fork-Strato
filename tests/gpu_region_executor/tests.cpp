// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <tuple>
#include <variant>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <common/mapping_identity.h>

using ContextTag = uint64_t;
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(T &single) : std::span<T>(&single, 1) {}
};
template<class... A> auto exception(const char *message, A &&...) { return std::runtime_error(message); }
using RecursiveSpinLock = std::recursive_mutex; // state-lock facade; no resource/GPU waits in this gate

struct AllocationControl { static inline bool fail{}; };
template<class T> struct NodeAllocator {
    using value_type = T;
    NodeAllocator() = default;
    template<class U> NodeAllocator(const NodeAllocator<U> &) {}
    T *allocate(size_t count) {
        if (AllocationControl::fail) throw std::bad_alloc{};
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T *pointer, size_t count) { std::allocator<T>{}.deallocate(pointer, count); }
    template<class U> bool operator==(const NodeAllocator<U> &) const { return true; }
};

namespace skyline::gpu {
    struct Buffer;
    struct Texture;
    struct TextureView;
    struct UsageTracker {};
    struct GPU {};
    struct FenceCycle {};
    struct TextureAliasReconciliationRequest {
        std::shared_ptr<Texture> target;
        std::shared_ptr<Buffer> bufferTarget;
        TextureAliasReconciliationRequest(std::shared_ptr<Texture> target = {}, std::vector<int> = {}) : target(std::move(target)) {}
    };
    namespace texture {
        struct Region { uintptr_t begin{}, end{}; uint64_t hostVersion{1}; };
        struct RegionAuthority { std::recursive_mutex mutex; std::vector<Region> regions; };
        struct Binding { size_t region{}; uint64_t residentVersion{1}; };
    }
    struct Buffer : std::enable_shared_from_this<Buffer> {
        std::atomic<bool> regionTracked{}, regionStale{};
        std::atomic<std::shared_ptr<texture::RegionAuthority>> regionAuthority;
        std::vector<texture::Binding> regionBindings;
        RecursiveSpinLock stateMutex;
        enum class DirtyState { Clean, CpuDirty, GpuDirty } dirtyState{DirtyState::Clean};
        ContextTag pendingRegionUseTag{};
        std::optional<span<uint8_t>> guest;
        unsigned preparations{}, writes{}, coherenceChecks{};
        std::vector<std::pair<vk::DeviceSize, vk::DeviceSize>> ranges;
        bool locked{};
        void RequireRegionCoherence(ContextTag) {
            ++coherenceChecks;
            if (regionStale) {
                TextureAliasReconciliationRequest request;
                request.bufferTarget = shared_from_this();
                throw request;
            }
        }
        void PrepareGpuWrite(UsageTracker &) { ++preparations; }
        void CommitGpuWrite(UsageTracker &, vk::DeviceSize offset, vk::DeviceSize size) {
            ++writes; ranges.emplace_back(offset, size); dirtyState = DirtyState::GpuDirty;
        }
        static void ValidateRegionWrites(span<struct BufferView>, span<TextureView *>);
    };
    struct BufferView {
        Buffer *buffer{};
        vk::DeviceSize offset{}, size{};
        explicit operator bool() const { return buffer; }
        Buffer *GetBuffer() const { return buffer; }
        vk::DeviceSize GetOffset() const { return offset; }
        bool LockWithTag(ContextTag) { return !std::exchange(buffer->locked, true); }
    };
    struct Texture : std::enable_shared_from_this<Texture> {
        std::atomic<bool> regionTracked{}, regionStale{};
        std::atomic<std::shared_ptr<texture::RegionAuthority>> regionAuthority;
        std::vector<texture::Binding> regionBindings;
        std::recursive_mutex stateMutex;
        enum class DirtyState { Clean, CpuDirty } dirtyState{DirtyState::Clean};
        unsigned writes{};
        void MarkGpuDirty(UsageTracker &) { ++writes; }
    };
    struct TextureView { std::shared_ptr<Texture> texture; };
    #include "validate.inc"

    namespace interconnect {
        namespace node {
            using Function = std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &)>;
            struct FunctionNode { Function function; explicit FunctionNode(Function &&f) : function(std::move(f)) {} };
            using Node = std::variant<FunctionNode>;
        }
        struct CommandExecutor {
            MappingIdentity::Epoch textureMappingEpoch;
            bool regionOperationActive{true};
            boost::container::small_vector<Texture *, 8> regionOperationUses;
            boost::container::small_vector<Buffer *, 8> regionOperationBufferUses;
            boost::container::small_vector<BufferView, 8> regionOperationBufferWrites;
            void TrackBufferRegionUse(Buffer &);
            void PrepareBufferWrite(BufferView);
            struct RegionPublication {
                MappingIdentity::Guard mappings;
                boost::container::small_vector<std::unique_lock<RecursiveSpinLock>, 8> bufferStates;
                boost::container::small_vector<std::unique_lock<std::recursive_mutex>, 8> states;
                boost::container::small_vector<std::unique_lock<std::recursive_mutex>, 4> metadata;
            };
            RegionPublication AcquireRegionPublication(span<TextureView *>, span<BufferView> = {});
            struct Slot { std::list<node::Node, NodeAllocator<node::Node>> nodes, pendingPostRenderPassNodes; };
            std::unique_ptr<Slot> slot{std::make_unique<Slot>()};
            bool renderPass{};
            ContextTag tag{1};
            UsageTracker usageTracker;
            std::vector<std::shared_ptr<Buffer>> attached;
            bool AttachBuffer(BufferView &);
            void AttachBufferBase(std::shared_ptr<Buffer> buffer) { attached.push_back(std::move(buffer)); }
            void PreflightTextureUses(span<TextureView *>, span<TextureView *>) {}
            std::function<void()> onBoundary;
            void FinishRenderPass() { if (onBoundary) onBoundary(); renderPass = false; }
            void AddOutsideRpCommand(node::Function &&, span<TextureView *> = {}, span<BufferView> = {});
            void AddPostRpCommand(node::Function &&, span<BufferView> = {});
        };
        #include "executor.inc"
        struct Pipeline {
            std::vector<BufferView> descriptorBufferReads, descriptorBufferWrites;
            void PreflightDescriptorBuffers(CommandExecutor &);
        };
        #include "cached.inc"
    }
}

using namespace skyline::gpu;
using skyline::gpu::interconnect::CommandExecutor;
auto Command() { return [](auto &, const auto &, auto &) {}; }

int main() {
    {
        auto buffer = std::make_shared<Buffer>();
        BufferView view{buffer.get(), 128, 64};
        CommandExecutor executor;
        executor.PrepareBufferWrite(view);
        assert(buffer->preparations == 1 && buffer->writes == 0 && executor.slot->nodes.empty());
        // Cancellation during later descriptor resolution must leave no writer.
        executor.regionOperationBufferWrites.clear();
        assert(buffer->writes == 0);
        executor.AddOutsideRpCommand(Command(), {}, span<BufferView>{view});
        assert(buffer->writes == 1 && executor.slot->nodes.size() == 1);
        assert(buffer->ranges.back() == std::make_pair(vk::DeviceSize{128}, vk::DeviceSize{64}));
        std::puts("PASS: descriptor cancellation has no writer; real node publication commits the exact view range");
    }
    {
        auto buffer = std::make_shared<Buffer>();
        BufferView view{buffer.get(), 0, 512};
        CommandExecutor executor;
        AllocationControl::fail = true;
        bool failed{};
        try { executor.AddOutsideRpCommand(Command(), {}, span<BufferView>{view}); }
        catch (const std::bad_alloc &) { failed = true; }
        AllocationControl::fail = false;
        assert(failed && buffer->writes == 0 && executor.slot->nodes.empty());
        executor.AddOutsideRpCommand(Command(), {}, span<BufferView>{view});
        assert(buffer->writes == 1 && executor.slot->nodes.size() == 1);
        std::puts("PASS: node allocation failure publishes no writer and retry records once");
    }
    {
        uint8_t bytes[1024]{};
        auto group = std::make_shared<texture::RegionAuthority>();
        group->regions.push_back({reinterpret_cast<uintptr_t>(bytes), reinterpret_cast<uintptr_t>(bytes + 512), 1});
        auto buffer = std::make_shared<Buffer>();
        buffer->guest = span<uint8_t>{bytes}; buffer->regionTracked = true; buffer->regionAuthority = group;
        buffer->regionBindings.push_back({0, 1});
        auto image = std::make_shared<Texture>(); image->regionTracked = true; image->regionAuthority = group;
        image->regionBindings.push_back({0, 1}); TextureView imageView{image}; TextureView *writer{&imageView};
        BufferView view{buffer.get(), 0, 512}; CommandExecutor executor;
        bool rejected{};
        try { executor.AddOutsideRpCommand(Command(), span<TextureView *>{writer}, span<BufferView>{view}); }
        catch (const std::runtime_error &) { rejected = true; }
        assert(rejected && buffer->writes == 0 && image->writes == 0 && executor.slot->nodes.empty());
        // A nonintersecting view is valid despite sharing a component.
        BufferView separate{buffer.get(), 512, 512};
        executor.AddOutsideRpCommand(Command(), span<TextureView *>{writer}, span<BufferView>{separate});
        assert(buffer->writes == 1 && image->writes == 1);
        std::puts("PASS: production mixed writer validation rejects one-cell conflicts before nodes or authority claims");
    }
    {
        auto buffer = std::make_shared<Buffer>();
        auto group = std::make_shared<texture::RegionAuthority>(); group->regions.push_back({0, 512, 1});
        buffer->regionTracked = true; buffer->regionAuthority = group; buffer->regionBindings.push_back({0, 1});
        BufferView view{buffer.get(), 32, 16}; CommandExecutor executor;
        interconnect::Pipeline pipeline; pipeline.descriptorBufferReads.push_back(view); pipeline.descriptorBufferWrites.push_back(view);
        pipeline.PreflightDescriptorBuffers(executor);
        executor.renderPass = true;
        executor.onBoundary = [&] { buffer->dirtyState = Buffer::DirtyState::CpuDirty; };
        bool retried{};
        try { executor.AddOutsideRpCommand(Command(), {}, executor.regionOperationBufferWrites); }
        catch (const TextureAliasReconciliationRequest &request) { retried = request.bufferTarget == buffer; }
        assert(retried && buffer->writes == 0 && executor.slot->nodes.empty());
        buffer->dirtyState = Buffer::DirtyState::Clean;
        executor.AddOutsideRpCommand(Command(), {}, executor.regionOperationBufferWrites);
        assert(buffer->writes == 1 && buffer->pendingRegionUseTag == executor.tag);
        executor.regionOperationBufferUses.clear(); executor.regionOperationBufferWrites.clear();
        pipeline.PreflightDescriptorBuffers(executor); // descriptor cache hit with no SyncDescriptors
        executor.AddOutsideRpCommand(Command(), {}, executor.regionOperationBufferWrites);
        assert(buffer->writes == 2 && buffer->coherenceChecks >= 4);
        std::puts("PASS: retained descriptors replay writes; late CPU invalidation rejects before publication");
    }
    {
        auto buffer = std::make_shared<Buffer>(); BufferView view{buffer.get(), 0, 4}; CommandExecutor executor;
        executor.renderPass = true;
        executor.AddPostRpCommand(Command(), span<BufferView>{view});
        assert(buffer->writes == 1 && executor.slot->nodes.empty() && executor.slot->pendingPostRenderPassNodes.size() == 1);
        std::puts("PASS: post-render-pass reports claim only after their deferred node is retained");
    }
}
