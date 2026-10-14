// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <vulkan/vulkan.hpp>

namespace skyline::gpu::texture {
    // Subtraction avoids overflow in base + count. Remaining-count sentinels are
    // valid only when the base itself is inside the backing.
    inline bool SubresourceRangeFits(uint32_t total, uint32_t base, uint32_t count) {
        return base < total && count && (count == VK_REMAINING_MIP_LEVELS || count <= total - base);
    }

    inline bool ViewRangeFits(vk::ImageType imageType, vk::Extent3D extent, vk::ImageCreateFlags flags,
                              uint32_t levels, uint32_t layers, vk::ImageViewType type,
                              const vk::ImageSubresourceRange &range) {
        if (!SubresourceRangeFits(levels, range.baseMipLevel, range.levelCount)) return false;
        const auto viewLevels{range.levelCount == VK_REMAINING_MIP_LEVELS ? levels - range.baseMipLevel : range.levelCount};
        uint32_t availableLayers{layers};
        if (imageType == vk::ImageType::e3D && (type == vk::ImageViewType::e2D || type == vk::ImageViewType::e2DArray)) {
            if (!(flags & vk::ImageCreateFlagBits::e2DArrayCompatible) || viewLevels != 1) return false;
            availableLayers = range.baseMipLevel < 32 ? std::max(1U, extent.depth >> range.baseMipLevel) : 1U;
        }
        if (!SubresourceRangeFits(availableLayers, range.baseArrayLayer, range.layerCount)) return false;
        const auto viewLayers{range.layerCount == VK_REMAINING_ARRAY_LAYERS ? availableLayers - range.baseArrayLayer : range.layerCount};
        switch (type) {
            case vk::ImageViewType::e1D: return imageType == vk::ImageType::e1D && viewLayers == 1;
            case vk::ImageViewType::e1DArray: return imageType == vk::ImageType::e1D;
            case vk::ImageViewType::e2D: return imageType != vk::ImageType::e1D && viewLayers == 1;
            case vk::ImageViewType::e2DArray: return imageType != vk::ImageType::e1D;
            case vk::ImageViewType::e3D: return imageType == vk::ImageType::e3D && range.baseArrayLayer == 0 && viewLayers == 1;
            case vk::ImageViewType::eCube:
            case vk::ImageViewType::eCubeArray:
                return imageType == vk::ImageType::e2D && (flags & vk::ImageCreateFlagBits::eCubeCompatible) &&
                       extent.width == extent.height && (type == vk::ImageViewType::eCube ? viewLayers == 6 : viewLayers % 6 == 0);
        }
        return false;
    }
}
