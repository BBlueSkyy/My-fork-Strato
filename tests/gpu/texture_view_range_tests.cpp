// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <limits>
#include <gpu/texture/view_range.h>

int main() {
    using skyline::gpu::texture::ViewRangeFits;
    const vk::Extent3D extent{128, 128, 1};
    const auto cube{vk::ImageCreateFlagBits::eCubeCompatible};
    vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 8, 0, 6};
    auto fits{[&](uint32_t levels, uint32_t layers, vk::ImageViewType type = vk::ImageViewType::eCube) {
        return ViewRangeFits(vk::ImageType::e2D, extent, cube, levels, layers, type, range);
    }};
    // Existing capture: matching guest mappings are not enough to admit this view.
    assert(!fits(1, 6));
    assert(fits(8, 6));
    range.baseMipLevel = 1;
    assert(!fits(8, 6));
    range.levelCount = 7;
    assert(fits(8, 6));
    range.baseArrayLayer = 1;
    assert(!fits(8, 6));
    range.baseArrayLayer = 0;
    range.layerCount = 5;
    assert(!fits(8, 6));
    assert(fits(8, 6, vk::ImageViewType::e2DArray));
    assert(!fits(8, 6, vk::ImageViewType::e2D));
    range.levelCount = 0;
    assert(!fits(8, 6, vk::ImageViewType::e2DArray));
    range.levelCount = VK_REMAINING_MIP_LEVELS;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    assert(fits(8, 6));
    range.baseMipLevel = std::numeric_limits<uint32_t>::max() - 1;
    range.levelCount = 8;
    assert(!fits(8, 6));
    range = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6};
    assert(!ViewRangeFits(vk::ImageType::e2D, extent, {}, 8, 6, vk::ImageViewType::eCube, range));
    assert(!ViewRangeFits(vk::ImageType::e2D, {128, 64, 1}, cube, 8, 6, vk::ImageViewType::eCube, range));
    assert(!fits(8, 6, vk::ImageViewType::e3D));
    // 2D views of a 3D mip address depth slices, not VkImage.arrayLayers.
    range = {vk::ImageAspectFlagBits::eColor, 1, 1, 2, 2};
    auto sliceFits{[&] {
        return ViewRangeFits(vk::ImageType::e3D, {16, 16, 8}, vk::ImageCreateFlagBits::e2DArrayCompatible,
                             5, 1, vk::ImageViewType::e2DArray, range);
    }};
    assert(sliceFits());
    range.layerCount = 3;
    assert(!sliceFits());
    range.layerCount = 2;
    range.levelCount = 2;
    assert(!sliceFits());
}
