// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only native VkImage identity labels for the Dead Cells PR #425.
// VK_EXT_debug_utils is optional. If it is unavailable the probe records
// name_applied=false and never guesses a RenderDoc ResourceId.
#pragma once
#include <vulkan/vulkan_raii.hpp>
#include <mutex>
#include "deadcells_vk_identity_format.h"

namespace skyline::gpu::deadcells_probe::vk_link {
    // Vulkan requires host-side synchronization of object-name modifications.
    inline std::mutex imageNamingMutex;
    [[nodiscard]] inline bool NameImage(const vk::raii::Device &device,
                                         vk::Image image, const std::string &label) noexcept {
        if (!image || label.empty()) return false;
        const auto *dispatcher{device.getDispatcher()};
        if (!dispatcher || !dispatcher->vkSetDebugUtilsObjectNameEXT) return false;
        std::scoped_lock namingLock{imageNamingMutex};
        const VkImage rawImage{static_cast<VkImage>(image)};
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = VK_OBJECT_TYPE_IMAGE;
        info.objectHandle = HandleBits(rawImage);
        info.pObjectName = label.c_str();
        return dispatcher->vkSetDebugUtilsObjectNameEXT(static_cast<VkDevice>(*device), &info) == VK_SUCCESS;
    }
}
