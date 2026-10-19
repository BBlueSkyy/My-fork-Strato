// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace skyline::gpu::feedback {
    // Vulkan-Headers v1.3.290 ABI. The project's pinned headers predate these
    // extensions, so keep their declarations local instead of changing the SDK.
    inline constexpr vk::ImageLayout Layout{static_cast<vk::ImageLayout>(1000339000)};
    inline constexpr vk::ImageUsageFlagBits ImageUsageBit{static_cast<vk::ImageUsageFlagBits>(0x00080000)};
    inline constexpr vk::DynamicState DynamicState{static_cast<vk::DynamicState>(1000524000)};
    inline constexpr char LayoutExtension[]{"VK_EXT_attachment_feedback_loop_layout"};
    inline constexpr char DynamicStateExtension[]{"VK_EXT_attachment_feedback_loop_dynamic_state"};
    struct LayoutFeatures {
        VkStructureType sType{static_cast<VkStructureType>(1000339000)};
        void *pNext{};
        VkBool32 attachmentFeedbackLoopLayout{};
    };
    struct DynamicStateFeatures {
        VkStructureType sType{static_cast<VkStructureType>(1000524000)};
        void *pNext{};
        VkBool32 attachmentFeedbackLoopDynamicState{};
    };
    using SetAttachmentFeedbackLoopEnable = void (VKAPI_PTR *)(VkCommandBuffer, VkImageAspectFlags);

    inline vk::ImageUsageFlags ImageUsage(vk::ImageUsageFlags usage, bool enabled) {
        if (enabled && (usage & vk::ImageUsageFlagBits::eSampled) &&
            (usage & (vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eDepthStencilAttachment)))
            usage |= ImageUsageBit;
        return usage;
    }

    inline bool RangesOverlap(const vk::ImageSubresourceRange &a, const vk::ImageSubresourceRange &b, bool image3D) {
        const auto overlap{[](uint32_t aBase, uint32_t aCount, uint32_t bBase, uint32_t bCount) {
            const auto end{[](uint32_t base, uint32_t count) -> uint64_t {
                return count == VK_REMAINING_MIP_LEVELS ? std::numeric_limits<uint64_t>::max() : uint64_t{base} + count;
            }};
            return uint64_t{aBase} < end(bBase, bCount) && uint64_t{bBase} < end(aBase, aCount);
        }};
        return bool(a.aspectMask & b.aspectMask) && overlap(a.baseMipLevel, a.levelCount, b.baseMipLevel, b.levelCount) &&
               (image3D || overlap(a.baseArrayLayer, a.layerCount, b.baseArrayLayer, b.layerCount));
    }

    template<typename View>
    bool ViewsOverlap(const View *a, const View *b) {
        return a && b && a->texture.get() == b->texture.get() &&
               RangesOverlap(a->range, b->range, a->texture->dimensions.depth > 1);
    }

    template<typename View, typename Sampled, typename Colors>
    std::vector<View *> FindImages(const Sampled &sampled, const Colors &colors, View *depth) {
        std::vector<View *> result;
        for (auto view : sampled) {
            if (!view) continue;
            const bool overlap{ViewsOverlap(view, depth) || std::any_of(colors.begin(), colors.end(),
                [&](const auto *color) { return ViewsOverlap(view, color); })};
            if (overlap && std::none_of(result.begin(), result.end(),
                [&](const auto *found) { return found->texture.get() == view->texture.get(); }))
                result.push_back(view);
        }
        return result;
    }

    template<typename Writes, typename Sampled, typename Images>
    void PatchDescriptorLayouts(const Writes &writes, const Sampled &sampled, const Images &images) {
        size_t index{};
        for (const auto &write : writes) {
            if (write.descriptorType == vk::DescriptorType::eStorageImage) {
                // The feedback layout does not permit storage-image accesses.
                // Transitions currently cover the whole backing, so even a
                // disjoint storage view of it must not retain GENERAL here.
                for (uint32_t element{}; element < write.descriptorCount; ++element)
                    if (std::any_of(images.begin(), images.end(), [&](const auto *image) {
                        return image->texture->OwnsView(write.pImageInfo[element].imageView);
                    }))
                        throw std::runtime_error("Storage image binding shares an attachment feedback backing");
                continue;
            }
            if (write.descriptorType != vk::DescriptorType::eCombinedImageSampler &&
                write.descriptorType != vk::DescriptorType::eSampledImage)
                continue;
            for (uint32_t element{}; element < write.descriptorCount; ++element, ++index) {
                if (index >= sampled.size()) throw std::runtime_error("Feedback descriptor tracking is incomplete");
                const auto *view{sampled[index]};
                if (view && std::any_of(images.begin(), images.end(),
                    [&](const auto *image) { return image->texture.get() == view->texture.get(); }))
                    const_cast<vk::DescriptorImageInfo &>(write.pImageInfo[element]).imageLayout = Layout;
            }
        }
        if (index != sampled.size()) throw std::runtime_error("Feedback descriptors require a complete update");
    }

    template<typename CommandBuffer>
    void RecordLayoutTransition(CommandBuffer &commands, vk::Image image, vk::ImageSubresourceRange range,
                                vk::ImageLayout oldLayout, vk::ImageLayout newLayout) {
        // The affected image can also have vertex/compute/transfer accesses.
        // Scope availability/visibility to that image, never to global memory.
        commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands,
            {}, {}, {}, vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = oldLayout, .newLayout = newLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image, .subresourceRange = range,
            });
    }

    template<typename Device, typename CommandBuffer>
    void SetDynamicState(const Device &device, CommandBuffer &commands, vk::ImageAspectFlags aspects) {
        const auto set{reinterpret_cast<SetAttachmentFeedbackLoopEnable>(device.getDispatcher()->vkGetDeviceProcAddr(
            static_cast<VkDevice>(*device), "vkCmdSetAttachmentFeedbackLoopEnableEXT"))};
        if (!set) throw std::runtime_error("Enabled attachment feedback loop command is unavailable");
        set(static_cast<VkCommandBuffer>(*commands), static_cast<VkImageAspectFlags>(aspects));
    }
}
