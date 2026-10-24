// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <thread>
#include <cstdlib>
#include <boost/container/small_vector.hpp>
#include <utility>
#include <condition_variable>
#include <cstring>
#include <gpu/texture/backing_promotion.h>
#include <gpu/texture/overlap_group.h>
#include <gpu/texture/image_format_policy.h>
#include <gpu/texture/view_range.h>
#include <gpu/texture/pitch_linear.h>
#include <gpu/texture/secondary_overlap.h>
#include <gpu/texture/overlap_index.h>
#include <gpu/texture/mapping_match.h>
#include <bit>
#include <chrono>
#include <numeric>
#include <vulkan/vulkan_raii.hpp>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

inline bool operator!(const vk::Extent3D &extent) { return !extent.width || !extent.height || !extent.depth; }
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;
namespace constant { inline constexpr size_t PageSize{4096}; }
using u8 = uint8_t;
using ContextTag = uint64_t;
template<typename T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> other) : std::span<T>(other) {}
    bool valid() const { return this->data() && this->size(); }
    bool contains(const span &other) const { return this->begin() <= other.begin() && this->end() >= other.end(); }
    bool operator<(const span &other) const { return this->data() < other.data(); }
};
template<typename... Args> std::runtime_error exception(const char *text, Args &&...) {
    return std::runtime_error(text);
}
namespace util {
    inline i64 GetTimeNs() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
    template<typename T,typename U> T *AlignDown(T *value,U alignment) { return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(value) & ~(static_cast<uintptr_t>(alignment)-1)); }
    template<typename T,typename U> T *AlignUp(T *value,U alignment) { return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(value)+alignment-1) & ~(static_cast<uintptr_t>(alignment)-1)); }
    template<typename T> constexpr T DivideCeil(T value, T divisor) { return (value + divisor - 1) / divisor; }
    template<typename T, typename U> constexpr T AlignUp(T value, U alignment) { return (value + alignment - 1) & ~(static_cast<T>(alignment) - 1); }
    template<typename T, typename U> constexpr T AlignDown(T value, U alignment) { return value & ~(static_cast<T>(alignment) - 1); }
    template<typename T, typename U> constexpr bool IsAligned(T value, U alignment) { return value % alignment == 0; }
}

#define TRACE_EVENT(...)

unsigned validationErrors{};
VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validationErrors;
        std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
}

namespace skyline::gpu {
    namespace texture {
        using Dimensions = vk::Extent3D;
        constexpr size_t SectorWidth{16}, GobWidth{64}, GobHeight{8}, SectorLinesInGob{32};
        enum class TileMode { Block, Pitch, Linear };
        enum class RenderPassUsage { None, Sampled, RenderTarget };
        #include "layout.inc"
    }
    #include "sizes.inc"
    namespace memory {
        struct StagingBuffer {
            vk::raii::DeviceMemory allocation{nullptr};
            vk::raii::Buffer buffer{nullptr};
            vk::Buffer vkBuffer{};
            size_t allocationSize{};
            size_t size() const { return allocationSize; }
            u8 *pointer{};
            u8 *data() { return pointer; }
        };
        struct Image {
            vk::raii::DeviceMemory allocation{nullptr};
            vk::raii::Image image{nullptr};
            vk::Image vkImage{};
            Image(vk::raii::DeviceMemory &&memory, vk::raii::Image &&resource)
                : allocation{std::move(memory)}, image{std::move(resource)}, vkImage{*image} {}
            Image(Image &&other) : allocation{std::move(other.allocation)}, image{std::move(other.image)},
                vkImage{std::exchange(other.vkImage, {})} {}
            u8 *data() { throw std::runtime_error("Test images use optimal tiling"); }
            Image &operator=(Image &&) = delete; // Catch the production ownership regression.
        };
    }

    struct FenceCycle {
        const vk::raii::Device &device;
        vk::raii::Fence fence;
        vk::raii::CommandBuffer commands;
        bool submitted{}, cancelled{};
        unsigned submitWaits{}, gpuWaits{};
        std::vector<std::shared_ptr<void>> dependencies;
        std::shared_ptr<FenceCycle> previous;
        static inline bool failAttachment{};
        FenceCycle(const vk::raii::Device &device, vk::raii::CommandBuffer &&commands)
            : device{device}, fence{device, vk::FenceCreateInfo{}}, commands{std::move(commands)} {}
        void WaitSubmit() { ++submitWaits; assert(submitted); }
        void Wait(bool destroy = false) {
            if (cancelled) return;
            assert(submitted);
            ++gpuWaits;
            assert(device.waitForFences(*fence, true, UINT64_MAX) == vk::Result::eSuccess);
            if (destroy) { dependencies.clear(); previous.reset(); }
        }
        void Cancel() { cancelled = true; dependencies.clear(); previous.reset(); }
        void AttachObject(const std::shared_ptr<void> &object) {
            if (failAttachment) throw std::bad_alloc{};
            dependencies.push_back(object);
        }
        template<typename... Args> void AttachObjects(const Args &...objects) { (AttachObject(objects), ...); }
        void ChainCycle(std::shared_ptr<FenceCycle> cycle) { previous = std::move(cycle); }
    };

    namespace cache {
        struct FramebufferCache {
            struct Key {
                std::variant<std::vector<vk::ImageView>, std::vector<int>> attachments;
            };
            // Ordered vector is a host boundary: same entry/erase API as production's map.
            std::vector<std::pair<Key, vk::raii::Framebuffer>> framebufferCache;
            std::mutex mutex;
            std::vector<vk::raii::Framebuffer> RetireViews(span<const vk::ImageView>);
        };
        #include "cache.inc"
    }

    struct Allocator {
        const vk::raii::Device &device;
        vk::PhysicalDeviceMemoryProperties properties;
        bool failAllocation{};
        unsigned count{};
        u32 MemoryType(u32 bits, vk::MemoryPropertyFlags flags = {}) const {
            for (u32 i{}; i < properties.memoryTypeCount; ++i)
                if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
            throw std::runtime_error("No memory type");
        }
        memory::Image AllocateImage(const vk::ImageCreateInfo &info) {
            if (failAllocation) throw std::bad_alloc{};
            ++count;
            vk::raii::Image image{device, info};
            auto requirements{image.getMemoryRequirements()};
            vk::raii::DeviceMemory memory{device, vk::MemoryAllocateInfo{
                .allocationSize = requirements.size, .memoryTypeIndex = MemoryType(requirements.memoryTypeBits),
            }};
            image.bindMemory(*memory, 0);
            return {std::move(memory), std::move(image)};
        }
        std::shared_ptr<memory::StagingBuffer> AllocateStagingBuffer(size_t size) {
            if (failAllocation) throw std::bad_alloc{};
            auto staging{std::make_shared<memory::StagingBuffer>()};
            staging->allocationSize = size;
            staging->buffer = vk::raii::Buffer{device, vk::BufferCreateInfo{.size = size,
                .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst}};
            auto requirements{staging->buffer.getMemoryRequirements()};
            staging->allocation = vk::raii::DeviceMemory{device, vk::MemoryAllocateInfo{
                .allocationSize = requirements.size, .memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)}};
            staging->buffer.bindMemory(*staging->allocation, 0);
            staging->vkBuffer = *staging->buffer;
            staging->pointer = static_cast<u8 *>(staging->allocation.mapMemory(0, size));
            return staging;
        }
    };

    struct Scheduler {
        const vk::raii::Device &device;
        vk::raii::Queue queue;
        vk::raii::CommandPool pool;
        std::vector<std::shared_ptr<FenceCycle>> cycles;
        bool failRecording{};
        unsigned submissions{};
        bool deferSubmission{};
        struct Active {
            std::shared_ptr<FenceCycle> cycle;
            std::shared_ptr<FenceCycle> GetFenceCycle() { return cycle; }
            vk::raii::CommandBuffer &operator*() { return cycle->commands; }
            vk::raii::CommandBuffer *operator->() { return &cycle->commands; }
        };
        Scheduler(const vk::raii::Device &device, u32 family) : device{device}, queue{device, family, 0},
            pool{device, vk::CommandPoolCreateInfo{.queueFamilyIndex = family}} {}
        Active AllocateCommandBuffer() {
            if (failRecording) throw std::bad_alloc{};
            auto buffers{device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
                .commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1,
            })};
            auto cycle{std::make_shared<FenceCycle>(device, std::move(buffers.front()))};
            cycles.push_back(cycle);
            return {cycle};
        }
        void SubmitCommandBuffer(const vk::raii::CommandBuffer &commands, std::shared_ptr<FenceCycle> cycle) {
            if (deferSubmission) return;
            queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &*commands}, *cycle->fence);
            cycle->submitted = true;
            ++submissions;
        }
        template<typename F> std::shared_ptr<FenceCycle> Submit(F &&record) {
            auto buffer{AllocateCommandBuffer()};
            buffer->begin(vk::CommandBufferBeginInfo{});
            record(*buffer);
            buffer->end();
            SubmitCommandBuffer(*buffer, buffer.cycle);
            return buffer.cycle;
        }
        void FlushPending() {
            deferSubmission=false;
            for (auto &cycle : cycles)
                if (!cycle->submitted && !cycle->cancelled) SubmitCommandBuffer(cycle->commands,cycle);
        }
        void Complete() {
            for (auto &cycle : cycles)
                if (cycle->submitted) cycle->Wait(true);
                else cycle->Cancel(); // Unused host-test buffers were never submitted.
        }
        ~Scheduler() { Complete(); }
    };

    struct GPU {
        const vk::raii::Device &vkDevice;
        const vk::raii::PhysicalDevice &vkPhysicalDevice;
        u32 vkQueueFamilyIndex;
        struct { bool supportsImageFormatList; } traits;
        Allocator memory;
        cache::FramebufferCache framebufferCache;
        Scheduler scheduler;
        struct Settings {
            std::shared_ptr<bool> freeGuestTextureMemory{std::make_shared<bool>(false)};
            std::shared_ptr<bool> enableFastGpuReadbackHack{std::make_shared<bool>(false)};
        };
        struct Process {
            struct Memory {
                unsigned discards{};
                span<u8> CreateMirror(span<u8> mapping) { return mapping; }
                span<u8> CreateMirrors(const std::vector<span<u8>> &mappings) {
                    size_t size{};
                    for (const auto &mapping : mappings) {
                        if (mapping.data() != mappings.front().data() + size)
                            throw std::runtime_error("Host mapping adapter requires physically adjacent segments");
                        size += mapping.size();
                    }
                    return {mappings.front().data(), size};
                }
                void FreeMemory(span<u8> mirror) { ++discards; std::fill(mirror.begin(), mirror.end(), 0); }
            } memory;
            struct Trap {
                std::recursive_mutex mutex;
                struct Callbacks { std::function<void()> prepare; std::function<bool()> read,write; };
                std::vector<Callbacks> callbacks;
                std::atomic<unsigned> changes{};
                int CreateTrap(const std::vector<span<u8>> &,std::function<void()> prepare,std::function<bool()> read,std::function<bool()> write) {
                    std::scoped_lock lock{mutex}; callbacks.push_back({std::move(prepare),std::move(read),std::move(write)});
                    return static_cast<int>(callbacks.size()-1);
                }
                void TrapRegions(int,bool) { std::scoped_lock lock{mutex}; ++changes; }
                void RemoveTrap(int) { ++changes; }
                void DeleteTrap(int) { std::scoped_lock lock{mutex}; ++changes; }
            } trap;
        };
        struct State {
            std::shared_ptr<Settings> settings{std::make_shared<Settings>()};
            std::shared_ptr<Process> process{std::make_shared<Process>()};
        } state;
        GPU(const vk::raii::Device &device, const vk::raii::PhysicalDevice &physical, u32 family, bool list)
            : vkDevice{device}, vkPhysicalDevice{physical}, vkQueueFamilyIndex{family}, traits{list}, memory{device, physical.getMemoryProperties()},
              scheduler{device, family} {}
    };
    struct NativeFormat {
        vk::Format vkFormat;
        vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor};
        size_t blockWidth{1}, blockHeight{1}, bpb{4};
        bool IsCompressed() const {
            return vkFormat == vk::Format::eBc1RgbaUnormBlock || vkFormat == vk::Format::eBc1RgbaSrgbBlock ||
                   vkFormat == vk::Format::eBc3UnormBlock || vkFormat == vk::Format::eBc3SrgbBlock;
        }
        size_t GetSize(u32 width, u32 height) const { return width * height * bpb; }
        size_t GetSize(vk::Extent3D extent) const { return GetSize(extent.width, extent.height) * extent.depth; }
        operator vk::Format() const { return vkFormat; }
        bool IsCompatible(const NativeFormat &) const { return true; } // Format classes are tested by real Vulkan.
        vk::ImageAspectFlags Aspect(bool) const { return vkAspect; }
    };
    namespace texture { using Format = NativeFormat *; }
    namespace format {
        inline NativeFormat rgba{vk::Format::eR8G8B8A8Unorm}, bgra{vk::Format::eB8G8R8A8Unorm};
        inline NativeFormat rgbaSrgb{vk::Format::eR8G8B8A8Srgb}, bgraSrgb{vk::Format::eB8G8R8A8Srgb};
        inline auto R8G8B8A8Unorm{&rgba}, B8G8R8A8Unorm{&bgra};
        inline auto R8G8B8A8Srgb{&rgbaSrgb}, B8G8R8A8Srgb{&bgraSrgb};
    }
    struct UsageTracker { struct Intervals { unsigned count{}; void Insert(span<u8>) { ++count; } } dirtyIntervals; };
    struct TextureView;
    struct GuestTexture {
        NativeFormat *format{};
        vk::Extent3D dimensions{128, 128, 1};
        u32 baseArrayLayer{}, mipLevelCount{3}, layerStride{90112};
        u32 viewMipBase{}, viewMipCount{8}, layerCount{6};
        vk::ImageViewType viewType{vk::ImageViewType::eCube};
        vk::ImageAspectFlags aspect{vk::ImageAspectFlagBits::eColor};
        vk::ComponentMapping swizzle{};
        struct TileConfig {
            texture::TileMode mode{texture::TileMode::Block}; size_t blockHeight{1}, blockDepth{1}, tileWidthSpacing{}; u32 pitch{};
            bool operator==(const TileConfig &) const = default;
        } tileConfig;
        using Mappings = std::vector<span<u8>>;
        Mappings mappings;
        bool MappingsValid() const {
            return std::all_of(mappings.begin(), mappings.end(), [](const auto &mapping) { return mapping.valid(); });
        }
        vk::ImageType GetImageType() const {
            if (viewType==vk::ImageViewType::e3D || dimensions.depth>1) return vk::ImageType::e3D;
            if (viewType==vk::ImageViewType::e1D || viewType==vk::ImageViewType::e1DArray) return vk::ImageType::e1D;
            return vk::ImageType::e2D;
        }
        u32 GetLayerStride() const { return layerStride; }
        size_t GetSize() const {
            if (tileConfig.mode == texture::TileMode::Pitch)
                return texture::PitchLinearTextureSize(CalculateLayerSize(), GetLayerStride(), layerCount, baseArrayLayer);
            return GetLayerStride() * (layerCount - baseArrayLayer);
        }
        u32 GetViewLayerCount() const { return layerCount; }
        u32 GetViewDepth() const { return dimensions.depth; }
        u32 CalculateLayerSize() const {
            if (tileConfig.mode == texture::TileMode::Pitch)
                return static_cast<u32>(dimensions.height * tileConfig.pitch);
            if (tileConfig.mode == texture::TileMode::Linear)
                return static_cast<u32>(format->GetSize(dimensions));
            auto layouts{texture::GetBlockLinearMipLayout(dimensions,1,1,4,1,1,4,tileConfig.blockHeight,tileConfig.blockDepth,mipLevelCount)};
            size_t bytes{}; for (const auto &level : layouts) bytes += level.blockLinearSize;
            return static_cast<u32>(bytes);
        }
    };

    class Texture : public std::enable_shared_from_this<Texture> {
      public:
        using BackingType = std::variant<vk::Image, memory::Image>;
        struct TextureViewStorage {
            vk::ImageViewType type;
            NativeFormat *format;
            vk::ComponentMapping mapping;
            vk::ImageSubresourceRange range;
            vk::raii::ImageView vkView;
            TextureViewStorage(vk::ImageViewType type, NativeFormat *format, vk::ComponentMapping mapping,
                               vk::ImageSubresourceRange range, vk::raii::ImageView &&view)
                : type{type}, format{format}, mapping{mapping}, range{range}, vkView{std::move(view)} {}
        };
        GPU &gpu;
        NativeFormat native;
        NativeFormat *format{&native};
        using Guest = GuestTexture;
        std::optional<Guest> guest{Guest{}};
        Texture(GPU &gpu, Guest value) : Texture(gpu,value.format->vkFormat,value.dimensions,value.mipLevelCount,value.layerCount) {
            guest=std::move(value);
            format=guest->format;
            dirtyState=DirtyState::CpuDirty;
        }
        void SetupGuestMappings();
        ~Texture() { if(HasAliases() || HasOverlaps()) SynchronizeGuest(true,false); } // Android unmapping remains the host boundary.
        BackingType backing;
        vk::ImageType backingImageType{vk::ImageType::e2D};
        vk::Extent3D dimensions{20, 12, 1};
        u32 levelCount{3}, layerCount{2};
        vk::SampleCountFlagBits sampleCount{vk::SampleCountFlagBits::e1};
        vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
        vk::ImageUsageFlags usage{vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment |
                                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst};
        vk::ImageCreateFlags flags;
        vk::ImageLayout layout{vk::ImageLayout::eUndefined};
        std::shared_ptr<FenceCycle> cycle;
        std::vector<TextureViewStorage> views;
        std::vector<vk::Format> backingViewFormats;
        u64 backingGeneration{1};
        std::condition_variable backingCondition;
        // Sentinels: backing promotion must not rewrite dirty/lifetime/alias identity state.
        enum DirtyState { Clean = 0, CpuDirty = 1, GpuDirty = 2 };
        DirtyState dirtyState{GpuDirty};
        bool memoryFreed{false};
        mutable std::recursive_mutex stateMutex;
        std::atomic<std::shared_ptr<texture::AliasGroup>> aliasGroup{};
        std::atomic<bool> aliasStale{};
        std::atomic<bool> overlapTracked{}, overlapStale{};
        std::shared_ptr<texture::OverlapState> overlapState;
        bool HasOverlaps() const { return overlapTracked.load(std::memory_order_acquire); }
        std::shared_ptr<texture::OverlapState> BuildOverlapState(u32 levels = 0);
        static void RegisterOverlaps(std::vector<std::shared_ptr<Texture>>);
        void ReconcileOverlaps(const std::vector<texture::GuestByteRange> *required = nullptr);
        std::atomic<bool> aliasRetired{};
        std::atomic<bool> aliasReadOnly{};
        u64 gpuWriteSerial{},gpuReadbackSerial{};
        bool replaced{};
        vk::PipelineStageFlags pendingStageMask{}, readStageMask{};
        std::recursive_mutex mutex;
        void lock() { mutex.lock(); }
        void unlock() { mutex.unlock(); }
        bool try_lock() { return mutex.try_lock(); }
        bool HasAliases() const { return static_cast<bool>(aliasGroup.load()); }
        bool CanReconcileGuestLayout(const Guest &) const;
        static bool CanMaterializeGuestLayout(const GuestTexture &);
        static bool IsGuestLayoutReversible(const GuestTexture &);
        static std::shared_ptr<texture::AliasGroup> RegisterAliases(std::vector<std::shared_ptr<Texture>>, const GuestTexture * = nullptr);
        void RequireAliasCoherence(ContextTag);
        void ReconcileAliases();
        void RecordGpuWrite();
        bool HasPendingOverlapWrite() const;
        void SynchronizeGuest(bool, bool);
        std::optional<int> trapHandle{777};
        u32 guestMemoryPins{};
        std::chrono::nanoseconds accumulatedGuestWaitTime{};
        static constexpr std::chrono::nanoseconds SkipReadbackHackWaitTimeThreshold{1'000'000};
        span<u8> mirror,alignedMirror;
        u64 accumulatedGuestWaitCounter{};
        static constexpr u64 SkipReadbackHackWaitCountThreshold{2};
        std::vector<texture::MipLevelLayout> mipLayouts;
        size_t surfaceSize{}, deswizzledSurfaceSize{};
        size_t deswizzledLayerStride{}, layerStride{};
        bool everUsedAsRt{};
        bool everUsedAsGpuWrite{};
        u32 lastRenderPassIndex{};
        texture::RenderPassUsage lastRenderPassUsage{texture::RenderPassUsage::None};
        void UpdateRenderPassUsage(u32,texture::RenderPassUsage);
        static void ValidateAliasWrites(span<TextureView *>);
        void MarkGpuDirty(UsageTracker &);
        std::shared_ptr<memory::StagingBuffer> downloadStagingBuffer;
        u64 mirrorHash{0x123456}, trapIdentity{777};
        Texture(GPU &gpu, vk::Format native, vk::Extent3D extent = {20, 12, 1}, u32 levels = 3, u32 layers = 2) : gpu{gpu}, native{native},
            dimensions{extent}, levelCount{levels}, layerCount{layers},
            flags{texture::GuestImageFormatFlags(native, true)} {
            guest->format = format;
            guest->dimensions = extent;
            guest->layerCount = layers;
            guest->mipLevelCount = levels;
            if (extent.width == extent.height && layers >= 6) flags |= vk::ImageCreateFlagBits::eCubeCompatible;
            mipLayouts = texture::GetBlockLinearMipLayout(extent, 1, 1, 4, 1, 1, 4, 1, 1, levels);
            surfaceSize = deswizzledSurfaceSize = CalculateLevelStride(mipLayouts) * layers;
            layerStride = deswizzledLayerStride = mipLayouts.front().linearSize;
            if (this->native.IsCompressed()) usage &= ~vk::ImageUsageFlagBits::eColorAttachment;
            if (gpu.vkPhysicalDevice.getFormatProperties(native).optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage)
                usage |= vk::ImageUsageFlagBits::eStorage;
            backing.emplace<memory::Image>(gpu.memory.AllocateImage(vk::ImageCreateInfo{
                .flags = flags, .imageType = vk::ImageType::e2D, .format = native, .extent = dimensions,
                .mipLevels = levelCount, .arrayLayers = layerCount, .samples = sampleCount,
                .tiling = tiling, .usage = usage,
            }));
        }
        vk::Image GetBacking() const {
            if (auto image{std::get_if<memory::Image>(&backing)}) return image->vkImage;
            return std::get<vk::Image>(backing);
        }
        void WaitOnBacking() { assert(GetBacking()); }
        span<const vk::Format> GetBackingViewFormats() const { return backingViewFormats; }
        void WaitOnFence() { if (cycle) { cycle->Wait(); cycle.reset(); } }
        bool ViewFormatAdmitted(vk::Format) const;
        void PromoteBackingForView(vk::Format, u32 levels = 0);
        bool ViewRangeFits(vk::ImageViewType, const vk::ImageSubresourceRange &, u32 levels = 0) const;
        bool CanGrowMipLevels(u32) const;
        std::shared_ptr<void> PinGuestMemoryForMipGrowth(u32);
        std::shared_ptr<void> PinGuestMemory();
        void SynchronizeHost(bool);
        void SynchronizeHostInline(const vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, bool);
        std::shared_ptr<memory::StagingBuffer> SynchronizeHostImpl();
        void CopyIntoStagingBuffer(const vk::raii::CommandBuffer &, const std::shared_ptr<memory::StagingBuffer> &);
        void CopyToGuest(u8 *);
        void EncodeGuest(u8 *,u8 *, GuestTexture * = nullptr, const std::vector<texture::MipLevelLayout> * = nullptr);
        void TransitionLayout(vk::ImageLayout);

        void FreeGuest();
        boost::container::small_vector<vk::BufferImageCopy, 10> GetBufferImageCopies();
        void CopyFromStagingBuffer(const vk::raii::CommandBuffer &, const std::shared_ptr<memory::StagingBuffer> &);
        std::shared_ptr<TextureView> GetView(vk::ImageViewType, vk::ImageSubresourceRange,
            texture::Format, vk::ComponentMapping, ContextTag);
        void SwapBacking(BackingType &&, vk::ImageCreateFlags, vk::ImageLayout, std::vector<vk::Format>);
    };
    struct TextureView {
        std::shared_ptr<Texture> texture;
        vk::ImageViewType type{vk::ImageViewType::e2DArray};
        NativeFormat *format;
        vk::ComponentMapping mapping{};
        vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 3, 0, 2};
        vk::ImageView vkView{};
        u64 vkViewGeneration{};
        vk::ImageView identityVkView{};
        u64 identityVkViewGeneration{};
        TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, NativeFormat *format)
            : texture{std::move(texture)}, type{type}, format{format} {}
        TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, vk::ImageSubresourceRange range,
                    NativeFormat *format, vk::ComponentMapping mapping)
            : texture{std::move(texture)}, type{type}, format{format}, mapping{mapping}, range{range} {}
        vk::ImageUsageFlags GetUsage() const;
        #include "view_api.inc"
        bool LockWithTag(ContextTag) { return false; } // Executor tag/resource-lock host boundary.
    };
    struct ContextLock { ContextLock(ContextTag, Texture &) {} };
    inline texture::Format ConvertHostCompatibleFormat(texture::Format format, const auto &) { return format; }
    #include "manager_class.inc"
    namespace texture {
        #include "depth_offset.inc"
    }
    #include "manager.inc"
    namespace bcn {
        // Converted formats are guarded out of alias reconciliation. These
        // declarations keep the native production synchronization bodies intact.
        template<typename... Args> void DecodeBc1(Args&&...) { throw std::runtime_error("BC conversion not exercised"); }
        template<typename... Args> void DecodeBc2(Args&&...) { throw std::runtime_error("BC conversion not exercised"); }
        template<typename... Args> void DecodeBc3(Args&&...) { throw std::runtime_error("BC conversion not exercised"); }
        template<typename... Args> void DecodeBc4(Args&&...) { throw std::runtime_error("BC conversion not exercised"); }
        template<typename... Args> void DecodeBc5(Args&&...) { throw std::runtime_error("BC conversion not exercised"); }
        template<typename... Args> void DecodeBc6(Args&&...) { throw std::runtime_error("BC conversion not exercised"); }
        template<typename... Args> void DecodeBc7(Args&&...) { throw std::runtime_error("BC conversion not exercised"); }
    }
    #include "guest_copy.inc"
    #include "production.inc"

void Texture::TransitionLayout(vk::ImageLayout pLayout) {
        WaitOnBacking();
        WaitOnFence();

        TRACE_EVENT("gpu", "Texture::TransitionLayout");

        if (layout != pLayout) {
            auto lCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commandBuffer) {
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eNoneKHR,
                    .dstAccessMask = vk::AccessFlagBits::eNoneKHR,
                    .oldLayout = std::exchange(layout, pLayout),
                    .newLayout = pLayout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = GetBacking(),
                    .subresourceRange = {
                        .aspectMask = format->vkAspect,
                        .levelCount = levelCount,
                        .layerCount = layerCount,
                    },
                });
            })};
            lCycle->AttachObject(shared_from_this());
            cycle = lCycle;
        }
    }

    namespace interconnect {
        struct CommandExecutor {
            ContextTag tag{1};
            std::shared_ptr<Texture> pending;
            std::vector<std::shared_ptr<Texture>> attachedTextures;
            UsageTracker usageTracker;
            bool AttachTexture(TextureView *);
            unsigned submissions{};
            std::function<void()> onSubmit;
            void Submit() { ++submissions; if (onSubmit) onSubmit(); }
            void HandleTextureBackingPromotion(const TextureBackingPromotionRequest &);
            void HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &);
        };
        #include "retry.inc"
        #include "attach.inc"
        #include "alias_retry.inc"
    }

    struct Buffer {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Buffer buffer{nullptr};
        size_t size;
        Buffer(GPU &gpu, size_t size) : size{size} {
            buffer = vk::raii::Buffer{gpu.vkDevice, vk::BufferCreateInfo{
                .size = size, .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            }};
            auto req{buffer.getMemoryRequirements()};
            memory = vk::raii::DeviceMemory{gpu.vkDevice, vk::MemoryAllocateInfo{
                .allocationSize = req.size, .memoryTypeIndex = gpu.memory.MemoryType(req.memoryTypeBits,
                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent),
            }};
            buffer.bindMemory(*memory, 0);
        }
        void Write(const std::vector<uint8_t> &bytes) {
            auto pointer{memory.mapMemory(0, size)};
            std::memcpy(pointer, bytes.data(), size);
            memory.unmapMemory();
        }
        std::vector<uint8_t> Read() {
            auto pointer{static_cast<uint8_t *>(memory.mapMemory(0, size))};
            std::vector<uint8_t> result{pointer, pointer + size};
            memory.unmapMemory();
            return result;
        }
    };

    void CheckImagelessAttachment(GPU &gpu, TextureView *view) {
        std::vector<vk::FramebufferAttachmentImageInfo> attachmentInfo;
        const auto viewLayers{view->range.layerCount};
        #include "attachment.inc" // Exact metadata emitted by RenderPassNode::AddAttachment.
        assert(attachmentInfo.front().usage == view->GetUsage());
        const vk::AttachmentDescription attachment{.format = *view->format, .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eLoad, .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eGeneral, .finalLayout = vk::ImageLayout::eGeneral};
        const vk::AttachmentReference reference{0, vk::ImageLayout::eGeneral};
        const vk::SubpassDescription subpass{.pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 1, .pColorAttachments = &reference};
        vk::raii::RenderPass renderPass{gpu.vkDevice, vk::RenderPassCreateInfo{.attachmentCount = 1,
            .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass}};
        const vk::FramebufferAttachmentsCreateInfo attachments{.attachmentImageInfoCount = 1,
            .pAttachmentImageInfos = attachmentInfo.data()};
        vk::raii::Framebuffer framebuffer{gpu.vkDevice, vk::FramebufferCreateInfo{.pNext = &attachments,
            .flags = vk::FramebufferCreateFlagBits::eImageless, .renderPass = *renderPass, .attachmentCount = 1,
            .width = attachmentInfo.front().width, .height = attachmentInfo.front().height,
            .layers = view->range.layerCount}};
        auto handle{view->GetView()};
        auto cycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,
                view->range.baseMipLevel, 1, view->range.baseArrayLayer, view->range.layerCount};
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                {}, {}, {}, vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite,
                    .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = view->texture->GetBacking(), .subresourceRange = range});
            const vk::RenderPassAttachmentBeginInfo beginAttachments{.attachmentCount = 1, .pAttachments = &handle};
            commands.beginRenderPass(vk::RenderPassBeginInfo{.pNext = &beginAttachments,
                .renderPass = *renderPass, .framebuffer = *framebuffer,
                .renderArea = vk::Rect2D{{0, 0}, {attachmentInfo.front().width, attachmentInfo.front().height}}}, vk::SubpassContents::eInline);
            commands.endRenderPass();
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eAllCommands,
                {}, {}, {}, vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eGeneral, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = view->texture->GetBacking(), .subresourceRange = range});
        })};
        cycle->Wait(true);
    }

    void CheckPromotion(GPU &gpu, bool reverse) {
        const auto native{reverse ? vk::Format::eB8G8R8A8Unorm : vk::Format::eR8G8B8A8Unorm};
        NativeFormat foreign{reverse ? vk::Format::eR8G8B8A8Unorm : vk::Format::eB8G8R8A8Unorm};
        NativeFormat integer{vk::Format::eR8G8B8A8Uint};
        NativeFormat srgb{reverse ? vk::Format::eB8G8R8A8Srgb : vk::Format::eR8G8B8A8Srgb};
        auto texture{std::make_shared<Texture>(gpu, native)};
        auto original{texture->GetBacking()};
        assert(!texture->flags && texture->ViewFormatAdmitted(native));
        assert(!texture->ViewFormatAdmitted(foreign.vkFormat));
        TextureView nativeView{texture, vk::ImageViewType::e2DArray, &texture->native};
        nativeView.range.levelCount = 1;
        auto oldView{nativeView.GetView()};
        TextureView illegalView{texture, vk::ImageViewType::e2DArray, &foreign};
        bool guarded{};
        try { illegalView.GetView(); } catch (const std::runtime_error &) { guarded = true; }
        assert(guarded); // Vulkan never receives the illegal request.

        const vk::AttachmentDescription attachment{.format = native, .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eLoad, .storeOp = vk::AttachmentStoreOp::eStore,
            .initialLayout = vk::ImageLayout::eGeneral, .finalLayout = vk::ImageLayout::eGeneral};
        const vk::AttachmentReference reference{0, vk::ImageLayout::eGeneral};
        const vk::SubpassDescription subpass{.pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 1, .pColorAttachments = &reference};
        vk::raii::RenderPass renderPass{gpu.vkDevice, vk::RenderPassCreateInfo{.attachmentCount = 1,
            .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass}};
        auto addFramebuffer{[&](vk::ImageView view) {
            vk::raii::Framebuffer framebuffer{gpu.vkDevice, vk::FramebufferCreateInfo{
                .renderPass = *renderPass, .attachmentCount = 1, .pAttachments = &view,
                .width = 20, .height = 12, .layers = 2}};
            const auto handle{*framebuffer};
            gpu.framebufferCache.framebufferCache.emplace_back(
                cache::FramebufferCache::Key{std::vector<vk::ImageView>{view}}, std::move(framebuffer));
            return handle;
        }};
        const auto oldFramebuffer{addFramebuffer(oldView)};
        auto unrelated{std::make_shared<Texture>(gpu, native)};
        TextureView unrelatedView{unrelated, vk::ImageViewType::e2DArray, &unrelated->native};
        unrelatedView.range.levelCount = 1;
        addFramebuffer(unrelatedView.GetView());
        gpu.framebufferCache.framebufferCache.emplace_back(
            cache::FramebufferCache::Key{std::vector<int>{1}}, vk::raii::Framebuffer{nullptr});

        std::vector<vk::BufferImageCopy> regions;
        size_t size{};
        for (const auto &copy : texture::PromotionCopyRegions(texture->dimensions, 3, 2)) {
            regions.push_back(vk::BufferImageCopy{.bufferOffset = size,
                .imageSubresource = copy.srcSubresource, .imageExtent = copy.extent});
            size += copy.extent.width * copy.extent.height * 4 * 2;
        }
        std::vector<uint8_t> bytes(size);
        for (size_t i{}; i < size; ++i) bytes[i] = static_cast<uint8_t>((i * 17 + i / 31) % 251);
        Buffer upload{gpu, size}, readback{gpu, size};
        upload.Write(bytes);
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 3, 0, 2};
        texture->cycle = gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {}, vk::ImageMemoryBarrier{.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = original, .subresourceRange = range});
            commands.copyBufferToImage(*upload.buffer, original, vk::ImageLayout::eTransferDstOptimal, regions);
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                {}, {}, {}, vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = original, .subresourceRange = range});
            commands.beginRenderPass(vk::RenderPassBeginInfo{.renderPass = *renderPass, .framebuffer = oldFramebuffer,
                .renderArea = vk::Rect2D{{0, 0}, {20, 12}}}, vk::SubpassContents::eInline);
            commands.endRenderPass();
        });
        texture->layout = vk::ImageLayout::eGeneral;
        auto previous{texture->cycle};

        // Exceptions at allocation and dependency preparation preserve backing/views.
        for (bool attachment : {false, true}) {
            gpu.memory.failAllocation = !attachment;
            FenceCycle::failAttachment = attachment;
            auto submissions{gpu.scheduler.submissions};
            bool failed{};
            try { texture->PromoteBackingForView(foreign.vkFormat); } catch (const std::bad_alloc &) { failed = true; }
            assert(failed && texture->GetBacking() == original && nativeView.GetView() == oldView);
            assert(texture->backingGeneration == 1 && !texture->flags && gpu.scheduler.submissions == submissions);
            gpu.memory.failAllocation = FenceCycle::failAttachment = false;
        }
        gpu.scheduler.failRecording = true;
        bool recordingFailed{};
        try { texture->PromoteBackingForView(foreign.vkFormat); } catch (const std::bad_alloc &) { recordingFailed = true; }
        assert(recordingFailed && texture->GetBacking() == original && nativeView.GetView() == oldView);
        assert(gpu.framebufferCache.framebufferCache.size() == 2); // Unrelated and imageless entries remain.
        assert(!previous->dependencies.empty()); // Evicted framebuffer lives on the still-unwaited old fence.
        gpu.scheduler.failRecording = false;
        unsigned attempts{}, resolved{};
        std::shared_ptr<TextureView> requestedView;
        texture::RetryBackingPromotion([&] {
            ++attempts;
            requestedView = texture->GetView(vk::ImageViewType::e2DArray, nativeView.range, &foreign, {}, 123);
        }, [&](const TextureBackingPromotionRequest &request) {
            ++resolved;
            assert(request.texture == texture && request.format == foreign.vkFormat);
            request.texture->PromoteBackingForView(request.format);
        });
        assert(attempts == 2 && resolved == 1 && requestedView && requestedView->GetView());
        assert(previous->submitWaits && previous->gpuWaits == 0);
        auto promoted{texture->GetBacking()};
        assert(promoted != original && texture->backingGeneration == 2);
        assert(texture->flags & vk::ImageCreateFlagBits::eMutableFormat);
        assert(texture->backingViewFormats.size() == (gpu.traits.supportsImageFormatList ? 2 : 0));
        assert(nativeView.GetView() != oldView && nativeView.vkViewGeneration == 2);
        illegalView.range = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 1, 1, 1, 1};
        assert(illegalView.GetView()); // Real nonzero mip/layer foreign view.
        auto generation{texture->backingGeneration};
        auto allocations{gpu.memory.count};
        texture->PromoteBackingForView(foreign.vkFormat);
        assert(texture->GetBacking() == promoted && texture->backingGeneration == generation && gpu.memory.count == allocations);
        texture->PromoteBackingForView(integer.vkFormat);
        assert(texture->backingGeneration == generation + (gpu.traits.supportsImageFormatList ? 1 : 0));
        assert(texture->backingViewFormats.size() == (gpu.traits.supportsImageFormatList ? 3 : 0));
        TextureView integerView{texture, vk::ImageViewType::e2DArray, &integer};
        assert(integerView.GetView() && illegalView.GetView() && nativeView.GetView());
        texture->PromoteBackingForView(srgb.vkFormat);
        TextureView srgbView{texture, vk::ImageViewType::e2DArray, &srgb};
        srgbView.range.levelCount = 1;
        assert(texture->usage & vk::ImageUsageFlagBits::eStorage);
        assert(!(srgbView.GetUsage() & vk::ImageUsageFlagBits::eStorage));
        assert(srgbView.GetView()); // A sampled sRGB alias must not inherit unsupported storage usage.
        CheckImagelessAttachment(gpu, &srgbView);
        TextureView srgbMipView{texture, vk::ImageViewType::e2DArray, &srgb};
        srgbMipView.range = {vk::ImageAspectFlagBits::eColor, 1, 1, 1, 1};
        CheckImagelessAttachment(gpu, &srgbMipView); // Restricted nonzero mip/layer render attachment.
        auto contentsCycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {}, vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead, .oldLayout = vk::ImageLayout::eGeneral,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = texture->GetBacking(), .subresourceRange = range});
            commands.copyImageToBuffer(texture->GetBacking(), vk::ImageLayout::eTransferSrcOptimal, *readback.buffer, regions);
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {},
                vk::BufferMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eHostRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = *readback.buffer, .size = size}, {});
        })};
        contentsCycle->Wait(true);
        assert(readback.Read() == bytes); // Distinct contents survive every mip/layer and both promotions.
        gpu.scheduler.Complete(); // Destroy copy dependencies; active allocation must survive.
        assert(texture->GetBacking() && nativeView.GetView() && illegalView.GetView());
        assert(texture->dirtyState == 2 && !texture->memoryFreed && texture->mirrorHash == 0x123456 && texture->trapIdentity == 777);
        Texture::BackingType replacement{std::in_place_type<memory::Image>, gpu.memory.AllocateImage(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D, .format = native, .extent = texture->dimensions,
            .mipLevels = 3, .arrayLayers = 2, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal, .usage = texture->usage})};
        generation = texture->backingGeneration;
        texture->SwapBacking(std::move(replacement), {}, vk::ImageLayout::eUndefined, {});
        assert(texture->backingGeneration == generation + 1 && !texture->flags && texture->backingViewFormats.empty());
        assert(nativeView.GetView() && nativeView.vkViewGeneration == generation + 1);
        guarded = false;
        try { illegalView.GetView(); } catch (const std::runtime_error &) { guarded = true; }
        assert(guarded); // Swap cannot inherit mutable permissions from an old backing.
        auto submissions{gpu.scheduler.submissions};
        texture->PromoteBackingForView(foreign.vkFormat);
        assert(texture->layout == vk::ImageLayout::eUndefined && gpu.scheduler.submissions == submissions);
        assert(illegalView.GetView()); // Undefined contents need no copy submission.
        gpu.framebufferCache.framebufferCache.clear(); // Destroy remaining framebuffer before its render pass/view.
    }

    void CheckCompressed(GPU &gpu, bool bc3, bool reverse) {
        const auto unorm{bc3 ? vk::Format::eBc3UnormBlock : vk::Format::eBc1RgbaUnormBlock};
        const auto srgb{bc3 ? vk::Format::eBc3SrgbBlock : vk::Format::eBc1RgbaSrgbBlock};
        auto texture{std::make_shared<Texture>(gpu, reverse ? srgb : unorm)};
        const auto image{texture->GetBacking()};
        const auto allocations{gpu.memory.count};
        NativeFormat alias{reverse ? unorm : srgb};
        assert(texture->flags & vk::ImageCreateFlagBits::eMutableFormat);
        texture->PromoteBackingForView(alias.vkFormat);
        assert(texture->GetBacking() == image && gpu.memory.count == allocations && texture->backingGeneration == 1);
        TextureView view{texture, vk::ImageViewType::e2DArray, &alias};
        view.range = {vk::ImageAspectFlagBits::eColor, 1, 1, 1, 1};
        assert(view.GetView());
        std::vector<vk::BufferImageCopy> regions;
        size_t size{};
        for (const auto &copy : texture::PromotionCopyRegions(texture->dimensions, 3, 2)) {
            regions.push_back(vk::BufferImageCopy{.bufferOffset = size,
                .imageSubresource = copy.srcSubresource, .imageExtent = copy.extent});
            size += ((copy.extent.width + 3) / 4) * ((copy.extent.height + 3) / 4) * (bc3 ? 16 : 8) * 2;
        }
        std::vector<uint8_t> bytes(size);
        for (size_t i{}; i < size; ++i) bytes[i] = static_cast<uint8_t>(i * 23 + 3);
        Buffer upload{gpu, size}, readback{gpu, size};
        upload.Write(bytes);
        auto cycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 3, 0, 2};
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {}, vk::ImageMemoryBarrier{.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = image, .subresourceRange = range});
            commands.copyBufferToImage(*upload.buffer, image, vk::ImageLayout::eTransferDstOptimal, regions);
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {}, vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead, .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = range});
            commands.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, *readback.buffer, regions);
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {},
                vk::BufferMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eHostRead, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = *readback.buffer, .size = size}, {});
        })};
        cycle->Wait(true);
        assert(readback.Read() == bytes);
    }

    void CheckMipGrowth(GPU &gpu, bool cpuDirty, size_t blockHeight) {
        auto texture{std::make_shared<Texture>(gpu, vk::Format::eR8G8B8A8Unorm, vk::Extent3D{128, 128, 1}, 1, 6)};
        texture->guest->tileConfig.blockHeight = blockHeight;
        texture->guest->viewType = vk::ImageViewType::e2DArray;
        texture->guest->viewMipCount = 1;
        auto layouts{texture::GetBlockLinearMipLayout(texture->dimensions, 1, 1, 4, 1, 1, 4, blockHeight, 1, 8)};
        texture->mipLayouts.assign(layouts.begin(), layouts.begin() + 1);
        std::vector<u8> guestBytes(90112 * 6, 0xCD), expected;
        std::vector<vk::BufferImageCopy> regions;
        size_t guestOffset{}, linearOffset{};
        for (u32 level{}; level < 8; ++level) {
            const auto &mip{layouts[level]};
            regions.push_back(vk::BufferImageCopy{.bufferOffset = linearOffset,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, level, 0, 6}, .imageExtent = mip.dimensions});
            for (u32 layer{}; layer < 6; ++layer) {
                std::vector<u8> bytes(mip.linearSize);
                for (size_t i{}; i < bytes.size(); ++i) bytes[i] = static_cast<u8>((level * 31 + layer * 17 + i * 7 + i / 23) % 251);
                texture::CopyLinearToBlockLinear(mip.dimensions, 1, 1, 4, mip.blockHeight, mip.blockDepth,
                    bytes.data(), guestBytes.data() + layer * 90112 + guestOffset);
                // Deliberately stale guest mip0 in the GPU-dirty case.
                if (!cpuDirty && !level)
                    for (auto &byte : bytes) byte ^= 0xA5;
                expected.insert(expected.end(), bytes.begin(), bytes.end());
            }
            guestOffset += mip.blockLinearSize;
            linearOffset += mip.linearSize * 6;
        }
        assert(guestOffset == 89088 && linearOffset == 524280);
        texture->mirror = guestBytes;
        texture->guest->mappings.push_back(texture->mirror);
        auto mappingIdentity{texture->guest->mappings.data()};
        const auto original{texture->GetBacking()};
        const vk::ImageSubresourceRange base{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6};
        const vk::ImageSubresourceRange complete{vk::ImageAspectFlagBits::eColor, 0, 8, 0, 6};
        TextureView cachedAlias{texture, vk::ImageViewType::e2DArray, base, texture->format, {}};
        const auto oldView{cachedAlias.GetView()};
        TextureView invalid{texture, vk::ImageViewType::eCube, complete, texture->format, {}};
        bool guarded{};
        try { invalid.GetView(); } catch (const std::runtime_error &) { guarded = true; }
        assert(guarded);
        guarded = false;
        try { texture->GetView(vk::ImageViewType::eCube, complete, texture->format, {}, 0); }
        catch (const std::runtime_error &) { guarded = true; }
        assert(guarded); // Both APIs guard the actual backing before Vulkan sees it.

        assert(texture->CanGrowMipLevels(8) && !texture->CanGrowMipLevels(9));
        texture->memoryFreed = true;
        assert(!texture->CanGrowMipLevels(8));
        texture->memoryFreed = false;
        texture->guest->layerStride = 65536;
        assert(!texture->CanGrowMipLevels(8));
        texture->guest->layerStride = 90112;
        texture->mirror = span<u8>{guestBytes.data(), guestBytes.size() - 1025};
        assert(!texture->CanGrowMipLevels(8));
        texture->mirror = guestBytes;
        texture->mipLayouts.front().blockHeight *= 2;
        assert(!texture->CanGrowMipLevels(8));
        texture->mipLayouts.front().blockHeight /= 2;

        Texture::Guest descriptor{*texture->guest};
        descriptor.viewType = vk::ImageViewType::eCube;
        descriptor.viewMipCount = descriptor.mipLevelCount = 8;
        if (cpuDirty) descriptor.viewMipCount = VK_REMAINING_MIP_LEVELS;
        auto fullMatchPredicate{[&](Texture::Guest guestTexture) {
            #include "request_validation.inc"
            auto &matchGuestTexture{*texture->guest};
            struct Candidate { std::shared_ptr<Texture> texture; } candidate{texture};
            auto hostMapping{&candidate};
            const auto requestedStride{guestTexture.GetLayerStride()};
            #include "request_range.inc"
            #include "match.inc"
        }};
        assert(fullMatchPredicate(descriptor));
        auto wrong{descriptor};
        wrong.layerCount = 7;
        assert(!fullMatchPredicate(wrong));
        wrong = descriptor;
        wrong.layerStride = 131072;
        assert(!fullMatchPredicate(wrong));
        wrong = descriptor;
        wrong.dimensions.width = 64;
        assert(!fullMatchPredicate(wrong));
        wrong = descriptor;
        wrong.viewMipBase = 8;
        wrong.viewMipCount = VK_REMAINING_MIP_LEVELS;
        guarded = false;
        try { fullMatchPredicate(wrong); } catch (const std::runtime_error &) { guarded = true; }
        assert(guarded); // A larger parent cannot legalize a base outside the guest chain.

        Buffer upload{gpu, texture->surfaceSize}, readback{gpu, linearOffset};
        upload.Write(std::vector<u8>{expected.begin(), expected.begin() + texture->surfaceSize});
        texture->cycle = gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                vk::ImageMemoryBarrier{.dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eUndefined, .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = original, .subresourceRange = base});
            commands.copyBufferToImage(*upload.buffer, original, vk::ImageLayout::eTransferDstOptimal, regions.front());
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal, .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = original, .subresourceRange = base});
        });
        texture->layout = vk::ImageLayout::eGeneral;
        texture->dirtyState = cpuDirty ? Texture::DirtyState::CpuDirty : Texture::DirtyState::GpuDirty;
        texture->downloadStagingBuffer = gpu.memory.AllocateStagingBuffer(texture->surfaceSize);
        auto previous{texture->cycle};

        // Allocation failure preserves backing, view generation and guest metadata.
        if (!cpuDirty) {
            gpu.memory.failAllocation = true;
            bool failed{};
            try { texture->PromoteBackingForView(texture->format->vkFormat, 8); } catch (const std::bad_alloc &) { failed = true; }
            gpu.memory.failAllocation = false;
            assert(failed && texture->GetBacking() == original && texture->levelCount == 1);
            assert(texture->guest->mipLevelCount == 1 && texture->backingGeneration == 1 && cachedAlias.GetView() == oldView);
        }

        interconnect::CommandExecutor executor;
        executor.pending = texture;
        *gpu.state.settings->freeGuestTextureMemory = true;
        std::shared_ptr<TextureView> cubeView;
        unsigned attempts{};
        texture::RetryBackingPromotion([&] {
            ++attempts;
            auto fullMatch{texture};
            const auto &guestTexture{descriptor};
            #include "request_range.inc"
            const ContextTag tag{1};
            auto select{[&]() -> std::shared_ptr<TextureView> {
                if (false) return {};
                #include "selection.inc"
                else return {};
            }};
            cubeView = select();
        }, [&](const auto &request) {
            assert(texture->guestMemoryPins == 1);
            const auto discards{gpu.state.process->memory.discards};
            executor.HandleTextureBackingPromotion(request);
            assert(gpu.state.process->memory.discards == discards); // Actual FreeGuest respected the lease during Submit.
        });
        *gpu.state.settings->freeGuestTextureMemory = false;
        assert(attempts == 2 && executor.submissions == 1 && texture->guestMemoryPins == 0);
        assert(texture->levelCount == 8 && texture->guest->mipLevelCount == 8 && !texture->downloadStagingBuffer);
        assert(texture->surfaceSize == linearOffset && texture->deswizzledSurfaceSize == linearOffset);
        assert(texture->guest->mappings.data() == mappingIdentity && texture->mirror.data() == guestBytes.data());
        assert(cubeView->texture == texture && cubeView->range.levelCount == 8 && texture->backingGeneration == 2 && cubeView->GetView());
        auto generation{texture->backingGeneration};
        auto allocations{gpu.memory.count};
        texture->PromoteBackingForView(texture->format->vkFormat, 8);
        assert(texture->backingGeneration == generation && gpu.memory.count == allocations);
        assert(cachedAlias.GetView() != oldView && cachedAlias.vkViewGeneration == 2);
        assert(previous->gpuWaits == 0 && previous->submitWaits);
        if (cpuDirty) assert(texture->cycle->previous != previous && texture->cycle->previous->previous == previous);
        else assert(texture->cycle->previous == previous);

        auto verifyBytes{[&] {
            auto cycle{gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                    vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferRead, .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = texture->GetBacking(), .subresourceRange = complete});
                commands.copyImageToBuffer(texture->GetBacking(), vk::ImageLayout::eGeneral, *readback.buffer, regions);
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, {},
                    vk::BufferMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite, .dstAccessMask = vk::AccessFlagBits::eHostRead,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .buffer = *readback.buffer, .size = linearOffset}, {});
            })};
            cycle->Wait(true);
            assert(readback.Read() == expected);
        }};
        verifyBytes();
        // A later write through the cached base-level alias must affect the cube
        // while leaving all added mips untouched.
        gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                vk::ImageMemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite, .oldLayout = vk::ImageLayout::eGeneral,
                    .newLayout = vk::ImageLayout::eGeneral, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = cachedAlias.texture->GetBacking(), .subresourceRange = base});
            const std::array<float, 4> color{1, 0, 1, 1};
            commands.clearColorImage(cachedAlias.texture->GetBacking(), vk::ImageLayout::eGeneral, vk::ClearColorValue{color}, base);
        });
        for (size_t i{}; i < 65536 * 6; ++i) expected[i] = i % 4 == 1 ? 0 : 255;
        verifyBytes();
        gpu.scheduler.Complete();
    }

    std::vector<u8> AliasHostBytes(const std::shared_ptr<Texture> &texture, const std::vector<u8> &encoded) {
        std::vector<u8> bytes(texture->surfaceSize);
        size_t guestOffset{}, hostOffset{};
        for (const auto &mip : texture->mipLayouts) {
            for (u32 layer{}; layer < texture->layerCount; ++layer)
                texture::CopyBlockLinearToLinear(mip.dimensions,1,1,4,mip.blockHeight,mip.blockDepth,
                    const_cast<u8 *>(encoded.data()) + layer * texture->guest->layerStride + guestOffset,
                    bytes.data() + hostOffset + layer * mip.linearSize);
            guestOffset += mip.blockLinearSize;
            hostOffset += mip.linearSize * texture->layerCount;
        }
        return bytes;
    }

    void VerifyAliasHost(const std::shared_ptr<Texture> &texture, const std::vector<u8> &encoded) {
        auto readback{texture->gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
        if (texture->cycle) texture->cycle->WaitSubmit();
        auto cycle{texture->gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            texture->CopyIntoStagingBuffer(commands,readback);
        })};
        cycle->AttachObjects(readback,texture);
        cycle->Wait(true);
        const auto expected{AliasHostBytes(texture,encoded)};
        assert(std::equal(expected.begin(),expected.end(),readback->data()));
    }

    void AliasRead(const std::shared_ptr<Texture> &texture) {
        unsigned retries{};
        texture::RetryBackingPromotion([&] {
            std::scoped_lock lock{*texture};
            texture->RequireAliasCoherence(1);
            texture->SynchronizeHost(false);
        }, [&](const auto &request) {
            using Request = std::remove_cvref_t<decltype(request)>;
            if constexpr (std::is_same_v<Request,TextureAliasReconciliationRequest>) {
                ++retries;
                request.target->ReconcileAliases();
            } else { assert(false); }
        });
        assert(retries <= 1); // No A/B reconstruction loop.
    }

    void EncodeAliasBase(const std::shared_ptr<Texture> &texture, std::vector<u8> &encoded, std::array<u8,4> color) {
        std::vector<u8> bytes(texture->mipLayouts.front().linearSize);
        for (size_t i{}; i < bytes.size(); ++i) bytes[i]=color[i%4];
        for (u32 layer{};layer<texture->layerCount;++layer)
            texture::CopyLinearToBlockLinear(texture->dimensions,1,1,4,texture->guest->tileConfig.blockHeight,1,
                bytes.data(),encoded.data()+layer*texture->guest->layerStride);
    }

    void WriteAliasRenderTarget(const std::shared_ptr<Texture> &texture, std::array<float,4> color, bool prepare = true,
                                std::optional<vk::Rect2D> clearRect = {}) {
        if (prepare) AliasRead(texture);
        std::scoped_lock lock{*texture};
        auto &gpu{texture->gpu};
        const vk::ImageSubresourceRange viewRange{vk::ImageAspectFlagBits::eColor,0,1,0,texture->layerCount};
        // A full overwrite can use its already-prepared backing without reading
        // old texels. Constructing the view here does not consult guest memory.
        auto view{prepare ? texture->GetView(vk::ImageViewType::e2DArray,viewRange,texture->format,{},0) :
            std::make_shared<TextureView>(texture,vk::ImageViewType::e2DArray,viewRange,texture->format,vk::ComponentMapping{})};
        const vk::AttachmentDescription attachment{.format=texture->format->vkFormat,.samples=vk::SampleCountFlagBits::e1,
            .loadOp=clearRect ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .initialLayout=vk::ImageLayout::eGeneral,.finalLayout=vk::ImageLayout::eGeneral};
        const vk::AttachmentReference reference{.attachment=0,.layout=vk::ImageLayout::eGeneral};
        const vk::SubpassDescription subpass{.pipelineBindPoint=vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount=1,.pColorAttachments=&reference};
        struct Resources {
            vk::raii::RenderPass pass{nullptr};
            vk::raii::Framebuffer framebuffer{nullptr};
            std::shared_ptr<TextureView> view;
        };
        auto resources{std::make_shared<Resources>()};
        resources->view=view;
        resources->pass=vk::raii::RenderPass{gpu.vkDevice,vk::RenderPassCreateInfo{
            .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass}};
        const auto handle{view->GetView()};
        resources->framebuffer=vk::raii::Framebuffer{gpu.vkDevice,vk::FramebufferCreateInfo{
            .renderPass=*resources->pass,.attachmentCount=1,.pAttachments=&handle,
            .width=texture->dimensions.width,.height=texture->dimensions.height,.layers=texture->layerCount}};
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,texture->layerCount};
        auto previous{texture->cycle};
        if (previous) previous->WaitSubmit();
        texture->cycle=gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eColorAttachmentOutput,{}, {}, {},
                vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,
                    .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                    .image=texture->GetBacking(),.subresourceRange=range});
            vk::ClearValue clear{vk::ClearColorValue{color}};
            commands.beginRenderPass(vk::RenderPassBeginInfo{.renderPass=*resources->pass,.framebuffer=*resources->framebuffer,
                .renderArea={{0,0},{texture->dimensions.width,texture->dimensions.height}},.clearValueCount=1,.pClearValues=&clear},
                vk::SubpassContents::eInline);
            if (clearRect) {
                const vk::ClearAttachment clearAttachment{.aspectMask=vk::ImageAspectFlagBits::eColor,
                    .colorAttachment=0,.clearValue=clear};
                const vk::ClearRect rect{.rect=*clearRect,.baseArrayLayer=0,.layerCount=texture->layerCount};
                commands.clearAttachments(clearAttachment,rect);
            }
            commands.endRenderPass();
        });
        texture->cycle->AttachObjects(resources,texture);
        texture->cycle->ChainCycle(previous);
        texture->UpdateRenderPassUsage(1,texture::RenderPassUsage::RenderTarget);
        texture->SynchronizeHost(true); // Actual executor submission dirty-state transition.
    }

    void CheckIndependentAliases(GPU &gpu) {
        std::vector<u8> guest(90112*6), expected;
        for (size_t i{};i<guest.size();++i) guest[i]=static_cast<u8>(i*17+i/101);
        expected=guest;
        auto make{[&](u32 stride,u32 levels,u32 layers=6) {
            auto texture{std::make_shared<Texture>(gpu,vk::Format::eR8G8B8A8Unorm,vk::Extent3D{128,128,1},levels,layers)};
            texture->guest->layerStride=stride;
            texture->guest->viewType=vk::ImageViewType::e2DArray;
            texture->guest->viewMipCount=levels;
            texture->mirror=guest;
            texture->guest->mappings.push_back(texture->mirror);
            texture->dirtyState=Texture::DirtyState::CpuDirty;
            return texture;
        }};
        auto a{make(90112,1)}, b{make(180224,1,3)}, c{make(90112,8)};
        for (auto t : {a,b,c}) { std::scoped_lock lock{*t}; t->SynchronizeHost(false); }
        auto aBacking{a->GetBacking()},bBacking{b->GetBacking()},cBacking{c->GetBacking()};
        auto cachedB{b->GetView(vk::ImageViewType::e2DArray,{vk::ImageAspectFlagBits::eColor,0,1,0,3},b->format,{},0)};
        const auto bView{cachedB->GetView()};
        Texture::RegisterAliases({c,b,a}); // Third representation was already initialized.
        bool conflicting{};
        AliasRead(a);
        auto aWrite{a->GetView(vk::ImageViewType::e2DArray,{vk::ImageAspectFlagBits::eColor,0,1,0,6},a->format,{},0)};
        auto bWrite{cachedB};
        std::array<TextureView *,2> writers{aWrite.get(),bWrite.get()};
        try { Texture::ValidateAliasWrites(writers); } catch(const std::runtime_error&) { conflicting=true; }
        assert(conflicting);
        writers[1]=aWrite.get(); Texture::ValidateAliasWrites(writers); // Multiple views of one backing are permitted.
        WriteAliasRenderTarget(a,{1,0,0,1});
        EncodeAliasBase(a,expected,{255,0,0,255});
        assert(guest!=expected && b->aliasStale && c->aliasStale);
        auto allocations{gpu.memory.count};
        AliasRead(b);
        assert(guest==expected && c->aliasStale && !b->aliasStale);
        VerifyAliasHost(b,expected);
        auto submissions{gpu.scheduler.submissions};
        AliasRead(b); AliasRead(b); AliasRead(a);
        assert(gpu.scheduler.submissions==submissions && gpu.memory.count==allocations);
        assert(cachedB->GetView()==bView); // No backing/view reconstruction on a representation switch.

        // Failure after source readback must leave the target upload retryable.
        WriteAliasRenderTarget(a,{1,0,1,1}); EncodeAliasBase(a,expected,{255,0,255,255});
        gpu.scheduler.failRecording=true;
        bool downloadFailed{}; try { b->ReconcileAliases(); } catch(const std::bad_alloc&) { downloadFailed=true; }
        gpu.scheduler.failRecording=false;
        assert(downloadFailed && a->dirtyState==Texture::DirtyState::GpuDirty && b->aliasStale);
        b->ReconcileAliases();
        gpu.memory.failAllocation=true;
        bool failed{}; try { AliasRead(b); } catch(const std::bad_alloc&) { failed=true; }
        gpu.memory.failAllocation=false;
        assert(failed && b->dirtyState==Texture::DirtyState::CpuDirty);
        AliasRead(b); VerifyAliasHost(b,expected);

        WriteAliasRenderTarget(b,{0,1,0,1});
        EncodeAliasBase(b,expected,{0,255,0,255});
        assert(a->aliasStale && c->aliasStale && guest!=expected);
        auto before{guest};
        { std::scoped_lock lock{*a}; a->SynchronizeGuest(false,true); }
        assert(guest==before); // An old GpuDirty alias cannot restore stale pixels.
        AliasRead(c);
        assert(guest==expected && a->aliasStale);
        VerifyAliasHost(c,expected); // All eight mips and all six faces use C's encoding.
        AliasRead(a); VerifyAliasHost(a,expected);
        WriteAliasRenderTarget(a,{0,0,1,1}); EncodeAliasBase(a,expected,{0,0,255,255});
        AliasRead(b); VerifyAliasHost(b,expected);
        assert(guest==expected);
        // CPU writes after the authoritative readback invalidate the writer too.
        { std::scoped_lock lock{*a}; a->SynchronizeGuest(true,true); }
        guest[0]^=0x5A; expected[0]^=0x5A;
        b->dirtyState=Texture::DirtyState::CpuDirty; // Host trap boundary: CPU page writes visit every exact-mapping trap.
        c->dirtyState=Texture::DirtyState::CpuDirty;
        AliasRead(c); VerifyAliasHost(c,expected);
        assert(a->GetBacking()==aBacking && b->GetBacking()==bBacking && c->GetBacking()==cBacking);
        assert(a->backingGeneration==1 && b->backingGeneration==1 && c->backingGeneration==1);
        assert(a->guest->mappings.front().data()==guest.data() && cachedB->GetView()==bView);
        gpu.scheduler.Complete();
        assert(!validationErrors);
    }

    void CheckManagerOverlapLookup(GPU &gpu) {
        // The host mirror adapter supports page-aligned, adjacent physical
        // segments. Actual disjoint segment order is checked by mapping_match.
        std::vector<u8> storage(131072 + 4096, 0x2D);
        auto *data{util::AlignUp(storage.data(), 4096)};
        TextureManager manager{gpu};
        GuestTexture descriptor;
        descriptor.format = format::R8G8B8A8Unorm;
        descriptor.dimensions = vk::Extent3D{128,128,1};
        descriptor.viewType = vk::ImageViewType::e2D;
        descriptor.mipLevelCount = descriptor.viewMipCount = descriptor.layerCount = 1;
        descriptor.layerStride = 65536;
        descriptor.mappings = {{data, 32768}, {data + 32768, 32768}};
        auto original{manager.FindOrCreate(descriptor)};
        const auto allocations{gpu.memory.count}, submissions{gpu.scheduler.submissions};
        assert(manager.FindOrCreate(descriptor)->texture == original->texture);
        assert(gpu.memory.count == allocations && gpu.scheduler.submissions == submissions);

        auto nested{descriptor};
        nested.dimensions = vk::Extent3D{32,32,1};
        nested.layerStride = 4096;
        nested.mappings = {{data + 4096, 4096}};
        auto small{manager.FindOrCreate(nested)};
        assert(small->texture != original->texture);
        assert(manager.FindOrCreate(descriptor)->texture == original->texture);
        assert(manager.FindOrCreate(nested)->texture == small->texture);

        // A new larger mapping must preserve a GPU writer contained inside it,
        // even though the writer cannot contain the requested primary mapping.
        auto source{nested};
        source.mappings = {{data + 98304, 4096}};
        auto writer{manager.FindOrCreate(source)};
        WriteAliasRenderTarget(writer->texture, {1,0,0,1});
        std::vector<u8> expected(4096, 0x2D);
        EncodeAliasBase(writer->texture, expected, {255,0,0,255});
        auto parent{nested};
        parent.dimensions = vk::Extent3D{64,64,1};
        parent.layerStride = 16384;
        parent.mappings = {{data + 94208, 16384}};
        auto larger{manager.FindOrCreate(parent)};
        assert(larger->texture != writer->texture);
        assert(std::equal(expected.begin(), expected.end(), data + 98304));
        assert(writer->texture->gpuReadbackSerial == writer->texture->gpuWriteSerial);
        gpu.scheduler.Complete();
    }

    void CheckManagerAliases(GPU &gpu) {
        TextureManager manager{gpu};
        std::vector<u8> guest(90112*6,0x2D),expected{guest};
        GuestTexture descriptor;
        descriptor.format=format::R8G8B8A8Unorm;
        descriptor.dimensions=vk::Extent3D{128,128,1}; descriptor.viewType=vk::ImageViewType::e2DArray;
        descriptor.mipLevelCount=descriptor.viewMipCount=1;
        descriptor.layerCount=6; descriptor.layerStride=90112;
        descriptor.mappings.push_back(span<u8>{guest});
        // The production manager must reject a missing mapping before front()
        // rather than reading an invalid span or populating the overlap map.
        {
            auto missing{descriptor};
            missing.mappings.clear();
            bool rejected{};
            try { manager.FindOrCreate(missing); }
            catch (const std::runtime_error &) { rejected = true; }
            assert(rejected);
            missing.mappings.push_back(span<u8>{});
            rejected = false;
            try { manager.FindOrCreate(missing); }
            catch (const std::runtime_error &) { rejected = true; }
            assert(rejected);
        }
        auto a{manager.FindOrCreate(descriptor)};
        { std::scoped_lock lock{*a->texture}; a->texture->SynchronizeHost(false); }
        WriteAliasRenderTarget(a->texture,{1,0,1,1});
        EncodeAliasBase(a->texture,expected,{255,0,255,255});
        auto different{descriptor}; different.layerCount=3; different.layerStride=180224;
        assert(different.layerCount*different.layerStride==descriptor.layerCount*descriptor.layerStride);
        std::shared_ptr<TextureView> b;
        unsigned registrations{};
        interconnect::CommandExecutor executor; executor.pending=a->texture;
        texture::RetryBackingPromotion([&] { b=manager.FindOrCreate(different,1); },[&](const auto &request) {
            using Request=std::remove_cvref_t<decltype(request)>;
            if constexpr(std::is_same_v<Request,TextureAliasReconciliationRequest>) {
                ++registrations;
                executor.HandleTextureBackingPromotion(request);
            } else { executor.HandleTextureBackingPromotion(request); }
        });
        assert(registrations==1 && b && b->texture!=a->texture && guest==expected);
        AliasRead(b->texture); VerifyAliasHost(b->texture,expected);
        assert(manager.FindOrCreate(different)->texture==b->texture);
        // Canonical mip growth still wins for the original stride, even though
        // the incompatible representation remains cached beside it.
        auto cube{descriptor}; cube.viewType=vk::ImageViewType::eCube;
        cube.mipLevelCount=cube.viewMipCount=8;
        auto c{manager.FindOrCreate(cube)};
        assert(c->texture==a->texture && c->texture->levelCount==8 && c->GetView());

        // A render target whose guest address starts at a non-zero mip must be
        // a view of the existing parent image, not a second independent backing.
        // The old matcher advanced levelMemOffset only when it already equalled
        // the requested address, so mip 1+ could never be reached.
        const u32 mipIndex{3};
        size_t mipOffset{};
        for (u32 i{}; i < mipIndex; ++i)
            mipOffset += c->texture->mipLayouts[i].blockLinearSize;
        auto mipRt{descriptor};
        mipRt.dimensions = c->texture->mipLayouts[mipIndex].dimensions;
        mipRt.viewType = vk::ImageViewType::e2D;
        mipRt.mipLevelCount = mipRt.viewMipCount = 1;
        mipRt.layerCount = 1;
        mipRt.layerStride = static_cast<u32>(c->texture->mipLayouts[mipIndex].blockLinearSize);
        mipRt.tileConfig.blockHeight = c->texture->mipLayouts[mipIndex].blockHeight;
        mipRt.tileConfig.blockDepth = c->texture->mipLayouts[mipIndex].blockDepth;
        mipRt.mappings.clear();
        mipRt.mappings.push_back(span<u8>{guest.data() + mipOffset, mipRt.layerStride});
        auto mipView{manager.FindOrCreate(mipRt)};
        assert(mipView->texture == c->texture);
        assert(mipView->range.baseMipLevel == mipIndex && mipView->range.levelCount == 1);
        assert(mipView->range.baseArrayLayer == 0 && mipView->range.layerCount == 1);
        WriteAliasRenderTarget(b->texture,{0,1,0,1}); EncodeAliasBase(b->texture,expected,{0,255,0,255});
        assert(manager.FindOrCreate(cube)->texture==c->texture); // No executor context: reconcile before the view lock.
        AliasRead(c->texture); VerifyAliasHost(c->texture,expected);
        auto allocations{gpu.memory.count};
        assert(manager.FindOrCreate(cube)->texture==c->texture);
        assert(manager.FindOrCreate(different)->texture==b->texture);
        assert(gpu.memory.count==allocations);
        // A third independently-addressed representation joins the same group.
        auto third{descriptor}; third.layerStride=270336; third.layerCount=2;
        auto d{manager.FindOrCreate(third)};
        assert(d->texture!=a->texture && d->texture!=b->texture);
        AliasRead(d->texture); VerifyAliasHost(d->texture,expected);
        WriteAliasRenderTarget(d->texture,{1,1,0,1}); EncodeAliasBase(d->texture,expected,{255,255,0,255});
        assert(a->texture->aliasStale && b->texture->aliasStale);
        AliasRead(a->texture); VerifyAliasHost(a->texture,expected);
        AliasRead(b->texture); VerifyAliasHost(b->texture,expected);
        const auto reads{gpu.scheduler.submissions};
        allocations=gpu.memory.count;
        for (unsigned i{};i<8;++i) {
            assert(manager.FindOrCreate(cube)->texture==a->texture);
            assert(manager.FindOrCreate(different)->texture==b->texture);
            assert(manager.FindOrCreate(third)->texture==d->texture);
            AliasRead(a->texture); AliasRead(b->texture); AliasRead(d->texture);
        }
        assert(gpu.scheduler.submissions==reads && gpu.memory.count==allocations);
        auto invalid{different}; invalid.layerStride=1;
        bool guarded{}; try { manager.FindOrCreate(invalid); } catch(const std::runtime_error&) { guarded=true; }
        assert(guarded); // The original safety guard is retained for impossible guest layouts.
        invalid=different; invalid.tileConfig.mode=texture::TileMode::Pitch; invalid.tileConfig.pitch=512;
        auto pitchAlias{manager.FindOrCreate(invalid)};
        assert(pitchAlias->texture->aliasReadOnly); // Existing policy permits materialization, never writer authority.
        invalid=different; invalid.tileConfig.mode=texture::TileMode::Linear;
        auto linearAlias{manager.FindOrCreate(invalid)};
        assert(linearAlias->texture->aliasReadOnly);
        gpu.scheduler.Complete();
    }

    // Exact mappings also find obsolete representations whose host format
    // cannot round-trip. They are not necessarily the current guest writer.
    void CheckHistoricalAlias(GPU &gpu, bool authoritative, bool writerReadBack, bool alreadyGrouped, bool executorContext, bool skippedReadback = false) {
        std::vector<u8> bytes(90112*6,0x39),expected{bytes};
        TextureManager manager{gpu}; // Guest mappings outlive every manager entry.
        GuestTexture descriptor;
        descriptor.format=format::R8G8B8A8Unorm; descriptor.dimensions=vk::Extent3D{128,128,1};
        descriptor.layerCount=6; descriptor.layerStride=90112; descriptor.mipLevelCount=descriptor.viewMipCount=1;
        descriptor.viewType=vk::ImageViewType::e2DArray; descriptor.mappings.push_back(span<u8>{bytes});
        auto oldDescriptor{descriptor};
        if (alreadyGrouped) { oldDescriptor.layerCount=3; oldDescriptor.layerStride=180224; }
        else oldDescriptor.dimensions.width=16;
        auto old{manager.FindOrCreate(oldDescriptor)};
        AliasRead(old->texture);
        WriteAliasRenderTarget(old->texture,{1,0,0,1});
        { std::scoped_lock lock{*old->texture}; old->texture->SynchronizeGuest(false,true); }
        auto current{manager.FindOrCreate(descriptor)};
        AliasRead(current->texture);
        if (alreadyGrouped) Texture::RegisterAliases({current->texture,old->texture});
        if (authoritative) {
            WriteAliasRenderTarget(old->texture,{0,0,1,1});
            if (writerReadBack) {
                std::scoped_lock lock{*old->texture}; old->texture->SynchronizeGuest(false,true);
            }
            expected=bytes;
        } else {
            WriteAliasRenderTarget(current->texture,{0,1,0,1});
            expected=bytes; EncodeAliasBase(current->texture,expected,{0,255,0,255});
            if (writerReadBack) {
                std::scoped_lock lock{*current->texture}; current->texture->SynchronizeGuest(false,true);
            }
            // Its old GPU contents are still present but superseded by current.
            old->texture->dirtyState=Texture::DirtyState::GpuDirty;
        }
        NativeFormat converted{*old->texture->format};
        old->texture->format=&converted; // Same allocation, non-reversible host representation.
        if (skippedReadback) {
            std::scoped_lock lock{*old->texture}; old->texture->SynchronizeGuest(false,true);
            assert(old->texture->gpuReadbackSerial < old->texture->gpuWriteSerial);
        }
        // The superseded historical image may no longer have guest backing of
        // its own. Only eligible aliases should acquire reconciliation leases.
        if (!authoritative) old->texture->memoryFreed=true;
        auto different{descriptor}; different.layerCount=3; different.layerStride=180224;
        assert(!old->texture->CanReconcileGuestLayout(different));
        assert(current->texture->CanReconcileGuestLayout(different));
        const auto before{bytes};
        std::shared_ptr<TextureView> next;
        bool rejected{};
        unsigned registrations{};
        interconnect::CommandExecutor executor;
        try {
            texture::RetryBackingPromotion([&] { next=manager.FindOrCreate(different,executorContext ? 1 : 0); },[&](const auto &request) {
                using Request=std::remove_cvref_t<decltype(request)>;
                if constexpr(std::is_same_v<Request,TextureAliasReconciliationRequest>) ++registrations;
                executor.HandleTextureBackingPromotion(request);
            });
        } catch (const std::runtime_error &) { rejected=true; }
        if (authoritative && !writerReadBack) {
            assert(rejected && !next && bytes==before);
            assert(!old->texture->aliasStale && !old->texture->replaced);
            // #392 keeps an actual GPU writer dirty when encoding is unavailable.
            // The retained-guest Clean exception applies only to never-written images.
            assert(old->texture->dirtyState==Texture::DirtyState::GpuDirty);
        } else {
            assert(!rejected && next && registrations==static_cast<unsigned>(executorContext) && bytes==expected);
            assert(old->texture->aliasStale && old->texture->replaced);
            assert(current->texture->aliasGroup.load()==next->texture->aliasGroup.load());
            { std::scoped_lock lock{*old->texture}; old->texture->SynchronizeGuest(false,true); }
            assert(bytes==expected); // A retired stale image cannot encode over the writer.
            bool cachedRejected{};
            try { old->texture->RequireAliasCoherence(1); } catch (const std::runtime_error &) { cachedRejected=true; }
            assert(cachedRejected); // Retained views cannot revive an obsolete writer.
            bool writeRejected{};
            try { old->texture->RecordGpuWrite(); } catch (const std::runtime_error &) { writeRejected=true; }
            assert(writeRejected);
            AliasRead(next->texture); VerifyAliasHost(next->texture,expected);
            AliasRead(current->texture); VerifyAliasHost(current->texture,expected);
            WriteAliasRenderTarget(next->texture,{1,1,0,1}); EncodeAliasBase(next->texture,expected,{255,255,0,255});
            AliasRead(current->texture); VerifyAliasHost(current->texture,expected);
            const auto submissions{gpu.scheduler.submissions},allocations{gpu.memory.count};
            for (unsigned i{};i<8;++i) {
                assert(manager.FindOrCreate(different)->texture==next->texture);
                assert(manager.FindOrCreate(descriptor)->texture==current->texture);
                AliasRead(next->texture); AliasRead(current->texture);
            }
            assert(gpu.scheduler.submissions==submissions && gpu.memory.count==allocations);
        }
        old->texture->format=old->texture->guest->format;
        old->texture->memoryFreed=false;
        gpu.scheduler.Complete();
    }

    void CheckOnlyHistoricalAliases(GPU &gpu, bool readBack, bool executorContext) {
        std::vector<u8> bytes(90112*6,0x58),expected{bytes};
        TextureManager manager{gpu};
        GuestTexture descriptor;
        descriptor.format=format::R8G8B8A8Unorm; descriptor.dimensions=vk::Extent3D{128,128,1};
        descriptor.layerCount=6; descriptor.layerStride=90112; descriptor.mipLevelCount=descriptor.viewMipCount=1;
        descriptor.viewType=vk::ImageViewType::e2DArray; descriptor.mappings.push_back(span<u8>{bytes});
        auto old{manager.FindOrCreate(descriptor)};
        AliasRead(old->texture); WriteAliasRenderTarget(old->texture,{1,0,1,1});
        EncodeAliasBase(old->texture,expected,{255,0,255,255});
        if (readBack) { std::scoped_lock lock{*old->texture}; old->texture->SynchronizeGuest(false,true); }
        NativeFormat converted{*old->texture->format}; old->texture->format=&converted;
        if (!readBack) { // #392 does not publish Clean for an unencodable real writer.
            std::scoped_lock lock{*old->texture}; old->texture->SynchronizeGuest(false,true);
            assert(old->texture->dirtyState==Texture::DirtyState::GpuDirty && bytes!=expected);
        }
        auto requested{descriptor}; requested.layerStride=180224; requested.layerCount=3;
        std::shared_ptr<TextureView> next;
        interconnect::CommandExecutor executor;
        bool rejected{};
        try {
            texture::RetryBackingPromotion([&] { next=manager.FindOrCreate(requested,executorContext ? 1 : 0); },[&](const auto &request) {
                executor.HandleTextureBackingPromotion(request);
            });
        } catch(const std::runtime_error &) { rejected=true; }
        if (!readBack) {
            assert(rejected && !next && !old->texture->aliasRetired && !old->texture->replaced);
            assert(bytes!=expected);
        } else {
            assert(!rejected && next && next->texture!=old->texture && old->texture->aliasRetired);
            assert(bytes==expected);
            AliasRead(next->texture); VerifyAliasHost(next->texture,expected);
            const auto submissions{gpu.scheduler.submissions},allocations{gpu.memory.count};
            for (unsigned i{};i<8;++i) {
                assert(manager.FindOrCreate(requested)->texture==next->texture);
                AliasRead(next->texture);
            }
            assert(gpu.scheduler.submissions==submissions && gpu.memory.count==allocations);
        }
        old->texture->format=old->texture->guest->format;
        gpu.scheduler.Complete();
    }

    void CheckHistoricalAliasReadbackFailure(GPU &gpu) {
        std::vector<u8> bytes(90112*6,0x42),expected{bytes};
        auto make{[&](u32 stride,u32 layers) {
            auto value{std::make_shared<Texture>(gpu,vk::Format::eR8G8B8A8Unorm,vk::Extent3D{128,128,1},1,layers)};
            value->guest->layerStride=stride; value->guest->viewType=vk::ImageViewType::e2DArray;
            value->guest->viewMipCount=1; value->guest->mappings.push_back(span<u8>{bytes});
            value->mirror=bytes; value->dirtyState=Texture::DirtyState::CpuDirty;
            return value;
        }};
        auto historical{make(90112,6)},current{make(180224,3)};
        WriteAliasRenderTarget(historical,{1,0,0,1});
        WriteAliasRenderTarget(current,{0,1,0,1}); EncodeAliasBase(current,expected,{0,255,0,255});
        NativeFormat converted{*historical->format}; historical->format=&converted;
        const auto before{bytes};
        gpu.scheduler.failRecording=true;
        bool failed{}; try { Texture::RegisterAliases({current,historical}); } catch (const std::bad_alloc &) { failed=true; }
        gpu.scheduler.failRecording=false;
        assert(failed && bytes==before && current->dirtyState==Texture::DirtyState::GpuDirty);
        assert(!historical->aliasStale && !historical->aliasRetired && !historical->replaced);
        assert(!current->aliasGroup.load());
        Texture::RegisterAliases({current,historical});
        assert(bytes==expected && historical->aliasRetired && historical->aliasStale);
        historical->format=historical->guest->format;
        gpu.scheduler.Complete();
    }

    void CheckAliasTraps(GPU &gpu) {
        TextureManager manager{gpu};
        std::vector<u8> bytes(90112*6,0x77),expected{bytes};
        GuestTexture descriptor;
        descriptor.format=format::R8G8B8A8Unorm; descriptor.dimensions=vk::Extent3D{128,128,1};
        descriptor.layerCount=6; descriptor.layerStride=90112; descriptor.mipLevelCount=descriptor.viewMipCount=1;
        descriptor.viewType=vk::ImageViewType::e2DArray; descriptor.mappings.push_back(span<u8>{bytes});
        auto a{manager.FindOrCreate(descriptor)};
        auto &trap{gpu.state.process->trap};
        assert(a->texture->trapHandle && trap.callbacks[*a->texture->trapHandle].read);
        WriteAliasRenderTarget(a->texture,{1,0,0,1}); EncodeAliasBase(a->texture,expected,{255,0,0,255});
        auto alternate{descriptor}; alternate.layerCount=3; alternate.layerStride=180224;
        auto b{manager.FindOrCreate(alternate)};
        AliasRead(b->texture);
        WriteAliasRenderTarget(a->texture,{0,0,1,1}); EncodeAliasBase(a->texture,expected,{0,0,255,255});
        auto group{a->texture->aliasGroup.load()};
        auto &callbacks{trap.callbacks[*a->texture->trapHandle]};
        // Finish just this writer's GPU cycle, as the real prepare callback does.
        callbacks.prepare();
        std::promise<void> locked,release;
        auto waitRelease{release.get_future()};
        auto thread{std::thread([&] { std::scoped_lock operation{group->operationMutex}; locked.set_value(); waitRelease.wait(); })};
        locked.get_future().wait();
        bool deferred{};
        { std::scoped_lock trapLock{trap.mutex}; deferred=!callbacks.read(); }
        release.set_value(); thread.join();
        assert(deferred); // No blocking group lock while the native trapMutex is held.
        { std::scoped_lock trapLock{trap.mutex}; assert(callbacks.read()); }
        assert(bytes==expected); // Callback retains a live weak_ptr from post-construction setup.
        AliasRead(b->texture); VerifyAliasHost(b->texture,expected);
        { std::scoped_lock trapLock{trap.mutex}; assert(callbacks.write()); }
        bytes[0]^=0xA5; expected[0]^=0xA5;
        const auto &otherCallbacks{trap.callbacks[*b->texture->trapHandle]};
        { std::scoped_lock trapLock{trap.mutex}; assert(otherCallbacks.write()); }
        AliasRead(a->texture); VerifyAliasHost(a->texture,expected);
        AliasRead(b->texture); VerifyAliasHost(b->texture,expected);
        gpu.scheduler.Complete();
    }

    void CheckCpuWriteBeforeSubmit(GPU &gpu) {
        TextureManager manager{gpu};
        std::vector<u8> bytes(90112*6,0x33),expected{bytes};
        GuestTexture descriptor;
        descriptor.format=format::R8G8B8A8Unorm; descriptor.dimensions=vk::Extent3D{128,128,1};
        descriptor.layerCount=6; descriptor.layerStride=90112; descriptor.mipLevelCount=descriptor.viewMipCount=1;
        descriptor.viewType=vk::ImageViewType::e2DArray; descriptor.mappings.push_back(span<u8>{bytes});
        auto a{manager.FindOrCreate(descriptor)};
        descriptor.layerCount=3; descriptor.layerStride=180224;
        auto b{manager.FindOrCreate(descriptor)};
        AliasRead(a->texture); AliasRead(b->texture);
        interconnect::CommandExecutor executor;
        executor.AttachTexture(b.get());
        auto &trap{gpu.state.process->trap};
        { std::scoped_lock trapLock{trap.mutex}; assert(trap.callbacks[*b->texture->trapHandle].write()); }
        bytes[0]^=0x5A; expected[0]^=0x5A;
        assert(b->texture->dirtyState==Texture::DirtyState::CpuDirty);
        auto commands{gpu.scheduler.AllocateCommandBuffer()};
        auto upload{commands.GetFenceCycle()};
        commands->begin(vk::CommandBufferBeginInfo{});
        b->texture->SynchronizeHostInline(*commands,upload,true);
        commands->end();
        gpu.scheduler.SubmitCommandBuffer(*commands,upload);
        b->texture->cycle=upload;
        VerifyAliasHost(b->texture,expected);
        gpu.scheduler.Complete();
    }

    void CheckRegistrationAfterReadback(GPU &gpu) {
        std::vector<u8> bytes(90112*6,0x44),expected{bytes};
        auto make{[&](u32 stride,u32 layers) {
            auto texture{std::make_shared<Texture>(gpu,vk::Format::eR8G8B8A8Unorm,vk::Extent3D{128,128,1},1,layers)};
            texture->guest->layerStride=stride; texture->guest->viewType=vk::ImageViewType::e2DArray;
            texture->guest->viewMipCount=1; texture->guest->mappings.push_back(span<u8>{bytes});
            texture->mirror=bytes; texture->dirtyState=Texture::DirtyState::CpuDirty;
            return texture;
        }};
        auto a{make(90112,6)},b{make(180224,3)};
        WriteAliasRenderTarget(a,{1,0,0,1});
        WriteAliasRenderTarget(b,{0,1,0,1}); EncodeAliasBase(b,expected,{0,255,0,255});
        { std::scoped_lock lock{*b}; b->SynchronizeGuest(false,true); }
        assert(bytes==expected && b->dirtyState==Texture::DirtyState::Clean);
        assert(a->dirtyState==Texture::DirtyState::GpuDirty && a->gpuWriteSerial<b->gpuWriteSerial);
        Texture::RegisterAliases({b,a});
        assert(bytes==expected); // Older GpuDirty cannot replace an already-read-back newer writer.
        NativeFormat converted{*b->format}; b->format=&converted;
        Texture::RegisterAliases({a,b},&*a->guest);
        assert(b->aliasRetired && a->aliasStale);
        // The highest surviving serial still belongs to a stale image. Adding
        // another eligible alias must keep guest bytes authoritative, not revive it.
        auto c{make(270336,2)};
        Texture::RegisterAliases({a,c});
        assert(a->aliasStale && c->aliasStale && bytes==expected);
        AliasRead(a); VerifyAliasHost(a,expected);
        AliasRead(c); VerifyAliasHost(c,expected);
        b->format=b->guest->format;
        gpu.scheduler.Complete();
    }

    void CheckSeparateLayouts(GPU &gpu) {
        auto texture{std::make_shared<Texture>(gpu, vk::Format::eR8G8B8A8Unorm, vk::Extent3D{128,128,1}, 1, 6)};
        auto guestTexture{*texture->guest};
        guestTexture.layerStride = 180224;
        guestTexture.layerCount = 3;
        guestTexture.viewType = vk::ImageViewType::e2DArray;
        guestTexture.viewMipCount = 1;
        const auto requestedStride{guestTexture.layerStride};
        #include "request_range.inc"
        auto &matchGuestTexture{*texture->guest};
        struct Candidate { std::shared_ptr<Texture> texture; } candidate{texture};
        auto hostMapping{&candidate};
        auto fullMatch{[&] {
            #include "match.inc"
        }};
        assert(!fullMatch()); // Both layouts fit their own images, but face offsets differ.
        auto unrepresentableMatch{texture};
        std::vector<std::shared_ptr<Texture>> exactAliases{texture};
        std::shared_ptr<texture::AliasGroup> separateGroup;
        std::vector<u8> bytes(90112*6);
        texture->mirror = bytes;
        texture->guest->mappings.push_back(texture->mirror);
        guestTexture.mappings = texture->guest->mappings;
        const ContextTag tag{};
        texture->dirtyState = Texture::DirtyState::CpuDirty;
        bool requestedReadOnlyAlias{};
        bool rejected{};
        try {
            #include "unrepresentable.inc"
        } catch (const std::runtime_error &) { rejected = true; }
        assert(!rejected); // A valid separate representation must have a coherent fallback.
        assert(!requestedReadOnlyAlias); // Both native block-linear layouts remain reversible.
    }

    void CheckMaterializationVsWriteback() {
        std::vector<u8> pitchBytes(64 * 32 * 4);
        GuestTexture pitch;
        pitch.format = format::R8G8B8A8Unorm;
        pitch.dimensions = vk::Extent3D{64,32,1};
        pitch.mipLevelCount = 1;
        pitch.viewMipCount = 1;
        pitch.layerCount = 1;
        pitch.layerStride = static_cast<u32>(pitchBytes.size());
        pitch.tileConfig.mode = texture::TileMode::Pitch;
        pitch.tileConfig.pitch = 64 * 4;
        pitch.mappings = {span<u8>{pitchBytes.data(), pitchBytes.size()}};
        assert(Texture::CanMaterializeGuestLayout(pitch));
        assert(!Texture::IsGuestLayoutReversible(pitch));

        auto multiPitch{pitch};
        multiPitch.mipLevelCount = 2;
        multiPitch.viewMipCount = 2;
        assert(!Texture::CanMaterializeGuestLayout(multiPitch));

        GuestTexture volume;
        volume.format = format::R8G8B8A8Unorm;
        volume.dimensions = vk::Extent3D{16,16,4};
        volume.viewType = vk::ImageViewType::e3D;
        volume.layerCount = 1;
        volume.mipLevelCount = 3;
        volume.viewMipCount = 3;
        volume.tileConfig.mode = texture::TileMode::Block;
        volume.tileConfig.blockHeight = 2;
        volume.tileConfig.blockDepth = 4;
        volume.layerStride = volume.CalculateLayerSize();
        std::vector<u8> volumeBytes(volume.layerStride);
        volume.mappings = {span<u8>{volumeBytes.data(), volumeBytes.size()}};
        assert(Texture::CanMaterializeGuestLayout(volume));
        assert(!Texture::IsGuestLayoutReversible(volume));

        auto array{pitch};
        array.tileConfig.mode = texture::TileMode::Block;
        array.tileConfig.blockHeight = 1;
        array.tileConfig.blockDepth = 1;
        array.mipLevelCount = 1;
        array.viewMipCount = 1;
        array.layerCount = 2;
        array.layerStride = array.CalculateLayerSize();
        std::vector<u8> arrayBytes(static_cast<size_t>(array.layerStride) * array.layerCount);
        array.mappings = {span<u8>{arrayBytes.data(), arrayBytes.size()}};
        assert(Texture::CanMaterializeGuestLayout(array));
        assert(Texture::IsGuestLayoutReversible(array));
    }

    void CheckFreedWriterGuestBridge(GPU &gpu) {
        std::vector<u8> bytes(90112 * 6), expected(bytes.size());
        for (size_t i{}; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(i * 29 + i / 113);
        expected = bytes;

        auto make{[&](u32 stride, u32 layers) {
            auto texture{std::make_shared<Texture>(gpu, vk::Format::eR8G8B8A8Unorm, vk::Extent3D{128,128,1}, 1, layers)};
            texture->guest->layerStride = stride;
            texture->guest->viewType = vk::ImageViewType::e2DArray;
            texture->guest->viewMipCount = 1;
            texture->guest->mappings.push_back(span<u8>{bytes});
            texture->mirror = bytes;
            texture->dirtyState = Texture::DirtyState::CpuDirty;
            { std::scoped_lock lock{*texture}; texture->SynchronizeHost(false); }
            return texture;
        }};

        auto writer{make(90112, 6)};
        auto alternate{make(180224, 3)};
        *gpu.state.settings->freeGuestTextureMemory = true;
        WriteAliasRenderTarget(writer, {0,0,1,1});
        EncodeAliasBase(writer, expected, {0,0,255,255});
        assert(writer->memoryFreed && writer->dirtyState == Texture::DirtyState::GpuDirty);
        assert(bytes != expected); // Fake FreeMemory removes the guest bridge.

        // FreeMemory discards padding too; only actual GPU texels can be restored.
        // Use the same eligible-first reference order as FindOrCreate.
        expected = bytes;
        EncodeAliasBase(writer, expected, {0,0,255,255});
        Texture::RegisterAliases({alternate, writer});
        auto group{writer->aliasGroup.load()};
        assert(group && alternate->aliasGroup.load() == group);
        assert(!writer->memoryFreed && writer->gpuReadbackSerial == writer->gpuWriteSerial);
        assert(bytes == expected && group->guestGeneration == group->generation);

        *gpu.state.settings->freeGuestTextureMemory = false;
        gpu.scheduler.Complete();
    }

    void CheckStorageWriterTracking(GPU &gpu) {
        std::vector<u8> bytes(64 * 64 * 4, 0x31);
        GuestTexture guest;
        guest.format = format::R8G8B8A8Unorm;
        guest.dimensions = vk::Extent3D{64,64,1};
        guest.mipLevelCount = 1;
        guest.viewMipCount = 1;
        guest.layerCount = 1;
        guest.layerStride = guest.CalculateLayerSize();
        guest.mappings = {span<u8>{bytes.data(), bytes.size()}};

        auto sampled{std::make_shared<Texture>(gpu, guest)};
        sampled->SetupGuestMappings();
        sampled->dirtyState = Texture::DirtyState::Clean;
        *gpu.state.settings->freeGuestTextureMemory = false;
        auto sampledCommands{gpu.scheduler.AllocateCommandBuffer()};
        sampled->SynchronizeHostInline(*sampledCommands, sampledCommands.GetFenceCycle(), true);
        assert(sampled->dirtyState == Texture::DirtyState::Clean && !sampled->everUsedAsGpuWrite);

        auto storage{std::make_shared<Texture>(gpu, guest)};
        storage->SetupGuestMappings();
        storage->dirtyState = Texture::DirtyState::Clean;
        UsageTracker usage;
        storage->MarkGpuDirty(usage);
        assert(storage->everUsedAsGpuWrite && usage.dirtyIntervals.count == 1);
        auto storageCommands{gpu.scheduler.AllocateCommandBuffer()};
        storage->SynchronizeHostInline(*storageCommands, storageCommands.GetFenceCycle(), true);
        assert(storage->dirtyState == Texture::DirtyState::GpuDirty);

        // The option's existing ownership policy is unchanged: enabling it may
        // still mark a sampled-only backing GPU-owned so guest pages can be freed.
        auto freeGuestSampled{std::make_shared<Texture>(gpu, guest)};
        freeGuestSampled->SetupGuestMappings();
        freeGuestSampled->dirtyState = Texture::DirtyState::Clean;
        *gpu.state.settings->freeGuestTextureMemory = true;
        auto freeCommands{gpu.scheduler.AllocateCommandBuffer()};
        freeGuestSampled->SynchronizeHostInline(*freeCommands, freeCommands.GetFenceCycle(), true);
        assert(freeGuestSampled->dirtyState == Texture::DirtyState::GpuDirty && !freeGuestSampled->everUsedAsGpuWrite);
        *gpu.state.settings->freeGuestTextureMemory = false;
    }

    void CheckReadOnlyAliasWriteGate(GPU &gpu) {
        std::vector<u8> bytes(90112 * 6, 0x4D);
        GuestTexture guest;
        guest.format = format::R8G8B8A8Unorm;
        guest.dimensions = vk::Extent3D{128,128,1};
        guest.mipLevelCount = 8;
        guest.viewMipCount = 8;
        guest.layerCount = 6;
        guest.layerStride = 90112;
        guest.mappings = {span<u8>{bytes.data(), bytes.size()}};
        assert(Texture::CanMaterializeGuestLayout(guest));
        assert(Texture::IsGuestLayoutReversible(guest));

        auto texture{std::make_shared<Texture>(gpu, guest)};
        texture->SetupGuestMappings();
        auto group{Texture::RegisterAliases({texture})};
        texture->aliasReadOnly = true;
        const auto generation{group->generation};
        const auto serial{texture->gpuWriteSerial};

        bool directRejected{};
        try { texture->RecordGpuWrite(); } catch (const std::runtime_error &) { directRejected = true; }
        assert(directRejected && group->generation == generation && texture->gpuWriteSerial == serial);

        UsageTracker usage;
        bool storageRejected{};
        try { texture->MarkGpuDirty(usage); } catch (const std::runtime_error &) { storageRejected = true; }
        assert(storageRejected && usage.dirtyIntervals.count == 0 && group->generation == generation);

        const auto oldPass{texture->lastRenderPassUsage};
        const auto oldPassIndex{texture->lastRenderPassIndex};
        bool rtRejected{};
        try { texture->UpdateRenderPassUsage(77, texture::RenderPassUsage::RenderTarget); } catch (const std::runtime_error &) { rtRejected = true; }
        assert(rtRejected && texture->lastRenderPassUsage == oldPass && texture->lastRenderPassIndex == oldPassIndex && !texture->everUsedAsRt);

        texture->dirtyState = Texture::DirtyState::GpuDirty;
        const auto before{bytes};
        texture->SynchronizeGuest(false, true);
        assert(bytes == before); // A read-only alias never becomes a GPU -> guest encoder.

        bool gpuDirtyRejected{};
        try { texture->SynchronizeHost(true); } catch (const std::runtime_error &) { gpuDirtyRejected = true; }
        assert(gpuDirtyRejected && group->generation == generation);

        // SubmitInternal conservatively supplies gpuDirty=true for all attached
        // textures, including sampled aliases. That synchronization hint must be
        // normalized before the read-only writer guard; actual writes are still
        // rejected by RecordGpuWrite/MarkGpuDirty/RT usage above.
        texture->dirtyState = Texture::DirtyState::Clean;
        auto submitBuffer{gpu.scheduler.AllocateCommandBuffer()};
        texture->SynchronizeHostInline(*submitBuffer, submitBuffer.GetFenceCycle(), true);
        assert(texture->dirtyState == Texture::DirtyState::Clean &&
               group->generation == generation && texture->gpuWriteSerial == serial);

        texture->dirtyState = Texture::DirtyState::CpuDirty;
    }

    void CheckLogicalExtent() {
        struct Guest {
            struct { u32 width, height, depth{1}; } dimensions;
            u32 depth, viewMipBase;
            u32 GetViewDepth() const { return depth; }
        };
        auto matches{[](Guest matchGuestTexture, Guest guestTexture) {
            #include "extent.inc"
        }};
        assert(!matches({{16, 1}, 1, 0}, {{1, 1}, 1, 0}));
        assert(!matches({{1, 1}, 1, 0}, {{16, 1}, 1, 0}));
        assert(matches({{16, 1}, 1, 0}, {{16, 1}, 1, 0}));
        assert(!matches({{20, 12}, 1, 1}, {{10, 6}, 1, 0}));
    }
}

int main() {
    using namespace skyline::gpu;
    vk::raii::Context context;
    const char *layer{"VK_LAYER_KHRONOS_validation"};
    const char *instanceExtensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    const vk::ValidationFeatureEnableEXT synchronization{vk::ValidationFeatureEnableEXT::eSynchronizationValidation};
    const vk::ValidationFeaturesEXT validation{.enabledValidationFeatureCount = 1, .pEnabledValidationFeatures = &synchronization};
    const vk::ApplicationInfo app{.apiVersion = VK_API_VERSION_1_1};
    vk::raii::Instance instance{context, vk::InstanceCreateInfo{.pNext = &validation, .pApplicationInfo = &app,
        .enabledLayerCount = 1, .ppEnabledLayerNames = &layer, .enabledExtensionCount = 2, .ppEnabledExtensionNames = instanceExtensions}};
    vk::raii::DebugUtilsMessengerEXT debug{instance, vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation, .pfnUserCallback = ValidationMessage}};
    {
        vk::raii::PhysicalDevice physical{std::move(instance.enumeratePhysicalDevices().front())};
        auto queues{physical.getQueueFamilyProperties()};
        u32 family{};
        while (!(queues.at(family).queueFlags & vk::QueueFlagBits::eGraphics)) ++family;
        const float priority{1};
        const vk::DeviceQueueCreateInfo queue{.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
        const char *extensions[]{VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME};
        const vk::PhysicalDeviceImagelessFramebufferFeatures imageless{.imagelessFramebuffer = true};
        const vk::PhysicalDeviceFeatures features{.textureCompressionBC = physical.getFeatures().textureCompressionBC};
        vk::raii::Device device{physical, vk::DeviceCreateInfo{.pNext = &imageless,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
            .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions, .pEnabledFeatures = &features}};
        for (bool formatList : {false, true}) {
            GPU gpu{device, physical, family, formatList};
            if (std::getenv("TEXMAN_LOOKUP_ONLY")) {
                CheckManagerOverlapLookup(gpu);
                continue;
            }
            CheckSeparateLayouts(gpu);
            CheckIndependentAliases(gpu);
            if (!std::getenv("TEXMAN_SKIP_LOOKUP")) CheckManagerOverlapLookup(gpu);
            CheckManagerAliases(gpu);
            for (bool authoritative : {false,true})
                for (bool readBack : {false,true})
                    for (bool grouped : {false,true})
                        for (bool context : {false,true}) CheckHistoricalAlias(gpu,authoritative,readBack,grouped,context);
            CheckHistoricalAlias(gpu,true,false,false,true,true);
            for (bool readBack : {false,true})
                for (bool context : {false,true}) CheckOnlyHistoricalAliases(gpu,readBack,context);
            CheckHistoricalAliasReadbackFailure(gpu);
            CheckAliasTraps(gpu);
            CheckCpuWriteBeforeSubmit(gpu);
            CheckRegistrationAfterReadback(gpu);
            CheckMaterializationVsWriteback();
            CheckFreedWriterGuestBridge(gpu);
            CheckStorageWriterTracking(gpu);
            CheckReadOnlyAliasWriteGate(gpu);
            for (bool reverse : {false, true}) CheckPromotion(gpu, reverse);
            for (bool cpuDirty : {false, true})
                for (size_t blockHeight : {1, 16}) CheckMipGrowth(gpu, cpuDirty, blockHeight);
            assert(features.textureCompressionBC);
            for (bool bc3 : {false, true})
                for (bool reverse : {false, true}) CheckCompressed(gpu, bc3, reverse);
        }
        CheckLogicalExtent();
    }
    assert(!validationErrors);
    if (std::getenv("TEXMAN_LOOKUP_ONLY")) {
        std::puts("PASS: production Texman fragmented reuse without allocations/submissions, nested lookup, contained GPU writer preservation; zero Vulkan validation errors");
        return 0;
    }
    std::puts("Production Texman: full FindOrCreate separate-layout fallback, read-only materialized alias write gates, three aliases, alternating RT writers, stale invalidation, historical non-reversible alias retirement/authority/rollback, real CPU traps/contention, failed download/upload recovery, test-only Vulkan transport, no redundant copies, views/fences; existing promotion and 1-to-8 mip expansion (6 faces) passed; zero Vulkan validation errors");
}
