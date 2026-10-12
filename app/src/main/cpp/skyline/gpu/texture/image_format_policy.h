// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vulkan/vulkan.hpp>

namespace skyline::gpu::texture {
    /**
     * These guest formats may be reinterpreted by the texture cache. The cost hint
     * must not remove the Vulkan permission needed by a differently formatted view.
     */
    constexpr bool RequiresMutableFormatForAliasing(vk::Format format) {
        switch (format) {
            case vk::Format::eR8G8B8A8Unorm:
            case vk::Format::eR8G8B8A8Snorm:
            case vk::Format::eR8G8B8A8Uscaled:
            case vk::Format::eR8G8B8A8Sscaled:
            case vk::Format::eR8G8B8A8Uint:
            case vk::Format::eR8G8B8A8Sint:
            case vk::Format::eR8G8B8A8Srgb:
            case vk::Format::eB8G8R8A8Unorm:
            case vk::Format::eB8G8R8A8Snorm:
            case vk::Format::eB8G8R8A8Uscaled:
            case vk::Format::eB8G8R8A8Sscaled:
            case vk::Format::eB8G8R8A8Uint:
            case vk::Format::eB8G8R8A8Sint:
            case vk::Format::eB8G8R8A8Srgb:
            case vk::Format::eA8B8G8R8UnormPack32:
            case vk::Format::eA8B8G8R8SnormPack32:
            case vk::Format::eA8B8G8R8UscaledPack32:
            case vk::Format::eA8B8G8R8SscaledPack32:
            case vk::Format::eA8B8G8R8UintPack32:
            case vk::Format::eA8B8G8R8SintPack32:
            case vk::Format::eA8B8G8R8SrgbPack32:
            case vk::Format::eBc1RgbaUnormBlock:
            case vk::Format::eBc1RgbaSrgbBlock:
            case vk::Format::eBc3UnormBlock:
            case vk::Format::eBc3SrgbBlock:
                return true;
            default:
                return false;
        }
    }

    constexpr vk::ImageCreateFlags GuestImageFormatFlags(vk::Format format, bool mutableFormatCostly) {
        return !mutableFormatCostly || RequiresMutableFormatForAliasing(format)
            ? vk::ImageCreateFlags{vk::ImageCreateFlagBits::eMutableFormat}
            : vk::ImageCreateFlags{};
    }
}
