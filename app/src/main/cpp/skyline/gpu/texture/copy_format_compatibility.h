// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <string_view>
#include <vulkan/vulkan_format_traits.hpp>
#include "resource_compatibility.h"

namespace skyline::gpu::texture {
    /**
     * @brief Whether Vulkan defines both color formats in the same compatibility class
     *
     * Requiring identical texel-block extents deliberately excludes Vulkan's wider
     * size-compatible copy rules that resize copy extents between compressed and
     * uncompressed formats. CopyOnly currently represents exact texel extents only.
     */
    constexpr bool AreCopyCompatibleFormats(
        vk::Format lhs, vk::ImageAspectFlags lhsAspect,
        vk::Format rhs, vk::ImageAspectFlags rhsAspect) {
        if (lhs == vk::Format::eUndefined || rhs == vk::Format::eUndefined ||
            lhsAspect != vk::ImageAspectFlagBits::eColor ||
            rhsAspect != vk::ImageAspectFlagBits::eColor ||
            vk::planeCount(lhs) != 1 || vk::planeCount(rhs) != 1 ||
            vk::blockExtent(lhs) != vk::blockExtent(rhs))
            return false;

        return std::string_view{vk::compatibilityClass(lhs)} ==
            std::string_view{vk::compatibilityClass(rhs)};
    }

    /**
     * @brief Classifies a pair after the caller has independently proved host-view support
     */
    constexpr FormatCompatibility ClassifyHostFormatCompatibility(
        vk::Format lhs, vk::ImageAspectFlags lhsAspect,
        vk::Format rhs, vk::ImageAspectFlags rhsAspect,
        bool viewCompatible) {
        if (lhs == rhs)
            return FormatCompatibility::Exact;
        if (viewCompatible)
            return FormatCompatibility::ViewCompatible;
        if (AreCopyCompatibleFormats(lhs, lhsAspect, rhs, rhsAspect))
            return FormatCompatibility::CopyCompatible;
        return FormatCompatibility::Incompatible;
    }
}
