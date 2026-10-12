// SPDX-License-Identifier: MPL-2.0
#include <utility>
#include <condition_variable>
#include <cstring>
#include <gpu/texture/backing_promotion.h>
#include <gpu/texture/image_format_policy.h>
#include <vulkan/vulkan_raii.hpp>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

using u32 = uint32_t;
using u64 = uint64_t;
using ContextTag = uint64_t;
template<typename T> using span = std::span<T>;
template<typename... Args> std::runtime_error exception(const char *text, Args &&...) {
    return std::runtime_error(text);
}

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
    namespace memory {
        struct Image {
            vk::raii::DeviceMemory allocation{nullptr};
            vk::raii::Image image{nullptr};
            vk::Image vkImage{};
            Image(vk::raii::DeviceMemory &&memory, vk::raii::Image &&resource)
                : allocation{std::move(memory)}, image{std::move(resource)}, vkImage{*image} {}
            Image(Image &&other) : allocation{std::move(other.allocation)}, image{std::move(other.image)},
                vkImage{std::exchange(other.vkImage, {})} {}
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
    };

    struct Scheduler {
        const vk::raii::Device &device;
        vk::raii::Queue queue;
        vk::raii::CommandPool pool;
        std::vector<std::shared_ptr<FenceCycle>> cycles;
        bool failRecording{};
        unsigned submissions{};
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
        void Complete() { for (auto &cycle : cycles) cycle->Wait(true); }
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
        GPU(const vk::raii::Device &device, const vk::raii::PhysicalDevice &physical, u32 family, bool list)
            : vkDevice{device}, vkPhysicalDevice{physical}, vkQueueFamilyIndex{family}, traits{list}, memory{device, physical.getMemoryProperties()},
              scheduler{device, family} {}
    };
    struct NativeFormat {
        vk::Format vkFormat;
        vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor};
        bool IsCompressed() const {
            return vkFormat == vk::Format::eBc1RgbaUnormBlock || vkFormat == vk::Format::eBc1RgbaSrgbBlock ||
                   vkFormat == vk::Format::eBc3UnormBlock || vkFormat == vk::Format::eBc3SrgbBlock;
        }
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
    struct TextureView;
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
        struct Guest {
            NativeFormat *format{};
            vk::ImageType GetImageType() const { return vk::ImageType::e2D; }
        };
        std::optional<Guest> guest{Guest{}};
        BackingType backing;
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
        int dirtyState{2};
        bool memoryFreed{false};
        u64 mirrorHash{0x123456}, trapIdentity{777};
        Texture(GPU &gpu, vk::Format native) : gpu{gpu}, native{native},
            flags{texture::GuestImageFormatFlags(native, true)} {
            guest->format = format;
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
        void PromoteBackingForView(vk::Format);
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
        TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, NativeFormat *format)
            : texture{std::move(texture)}, type{type}, format{format} {}
        TextureView(std::shared_ptr<Texture> texture, vk::ImageViewType type, vk::ImageSubresourceRange range,
                    NativeFormat *format, vk::ComponentMapping mapping)
            : texture{std::move(texture)}, type{type}, format{format}, mapping{mapping}, range{range} {}
        vk::ImageUsageFlags GetUsage() const;
        vk::ImageView GetView();
    };
    #include "production.inc"

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

    void CheckLogicalExtent() {
        struct Guest {
            struct { u32 width, height; } dimensions;
            u32 depth, viewMipBase;
            u32 GetViewDepth() const { return depth; }
        };
        auto matches{[](Guest matchGuestTexture, Guest guestTexture) {
            #include "extent.inc"
        }};
        assert(!matches({{16, 1}, 1, 0}, {{1, 1}, 1, 0}));
        assert(!matches({{1, 1}, 1, 0}, {{16, 1}, 1, 0}));
        assert(matches({{16, 1}, 1, 0}, {{16, 1}, 1, 0}));
        assert(matches({{20, 12}, 1, 1}, {{10, 6}, 1, 0}));
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
            for (bool reverse : {false, true}) CheckPromotion(gpu, reverse);
            assert(features.textureCompressionBC);
            for (bool bc3 : {false, true})
                for (bool reverse : {false, true}) CheckCompressed(gpu, bc3, reverse);
        }
        CheckLogicalExtent();
    }
    assert(!validationErrors);
    std::puts("Production backing promotion: RGBA/BGRA, pixels (3 mips x 2 layers), views/generation/reuse, format-list expansion, sRGB/storage legality, framebuffer retirement, pre-submit failures, SwapBacking, BC1/BC3 and logical extents passed; zero Vulkan validation errors");
}
