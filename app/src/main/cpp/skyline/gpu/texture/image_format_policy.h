// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vulkan/vulkan.hpp>

namespace skyline::gpu::texture {
    /**
     * Compressed aliases retain their conservative allocation. Ordinary color
     * images acquire mutable permission only when an actual foreign view is requested.
     */
    constexpr bool RequiresMutableFormatForAliasing(vk::Format format) {
        switch (format) {
            case vk::Format::eBc1RgbaUnormBlock:
            case vk::Format::eBc1RgbaSrgbBlock:
            case vk::Format::eBc3UnormBlock:
            case vk::Format::eBc3SrgbBlock:
                return true;
            default:
                return false;
        }
    }

    constexpr vk::ImageCreateFlags GuestImageFormatFlags(vk::Format format, bool mutableFormatCostly,
                                                         bool canPromote = true, bool mayHaveForeignView = true) {
        // Vulkan depth/stencil formats are only compatible with themselves.
        // They cannot legally acquire a foreign-format view.
        return mayHaveForeignView && (!mutableFormatCostly || !canPromote || RequiresMutableFormatForAliasing(format))
            ? vk::ImageCreateFlags{vk::ImageCreateFlagBits::eMutableFormat}
            : vk::ImageCreateFlags{};
    }
}
