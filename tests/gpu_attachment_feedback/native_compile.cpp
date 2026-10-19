// SPDX-License-Identifier: MPL-2.0
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <gpu/attachment_feedback.h>
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <array>
#include <string_view>

// Compile against the real pinned Vulkan-Hpp RAII dispatchers. The behavioral
// tests replace command recording, so would not catch ABI/overload mismatches.
void Record(vk::raii::Device &device, vk::raii::CommandBuffer &commands) {
    skyline::gpu::feedback::SetDynamicState(device, commands, {});
    skyline::gpu::feedback::RecordLayoutTransition(commands, {}, {}, vk::ImageLayout::eGeneral,
                                                   skyline::gpu::feedback::Layout);
}

struct Traits { bool supportsAttachmentFeedbackLoop{}; };
void Features(const vk::raii::PhysicalDevice &physicalDevice, Traits &traits) {
    namespace feedback = skyline::gpu::feedback;
    vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceRobustness2FeaturesEXT> enabledFeatures2;
    auto deviceExtensions{physicalDevice.enumerateDeviceExtensionProperties()};
    std::vector<std::array<char, VK_MAX_EXTENSION_NAME_SIZE>> enabledExtensions;
#include "features.inc"
}
