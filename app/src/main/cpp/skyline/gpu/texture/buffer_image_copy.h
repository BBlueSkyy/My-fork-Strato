// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <limits>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace skyline::gpu::texture {
    // An already-proved, fully represented GOB in a native RGBA8 2D image.
    // These are buffer offsets, not guest addresses or mapping identities.
    struct RawRgba8Gob {
        uint64_t bufferOffset{};
        uint32_t x{}, y{};
    };

    // Geometry only: callers must separately validate live physical identity,
    // ownership, format, sample count, subresource bounds and resource lifetimes.
    // No commands or authority are published on rejection.
    inline std::optional<std::vector<vk::BufferImageCopy>> PlanRawRgba8GobCopies(
        const std::vector<RawRgba8Gob> &gobs, uint64_t bufferSize, vk::Extent3D extent) {
        constexpr size_t MaximumGobs{128}; // 4096 Vulkan rectangles / transfer.
        if (gobs.size() > MaximumGobs || !extent.width || !extent.height || extent.depth != 1 ||
            extent.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            extent.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            return std::nullopt;
        for (size_t i{}; i < gobs.size(); ++i) {
            const auto &gob{gobs[i]};
            if (gob.bufferOffset % 4 || gob.bufferOffset > bufferSize || bufferSize - gob.bufferOffset < 512 ||
                gob.x % 16 || gob.y % 8 || gob.x > extent.width || extent.width - gob.x < 16 ||
                gob.y > extent.height || extent.height - gob.y < 8)
                return std::nullopt;
            for (size_t j{}; j < i; ++j) {
                const auto &other{gobs[j]};
                // Both validated intervals fit the buffer, so endpoint addition is safe.
                if ((gob.bufferOffset < other.bufferOffset + 512 && other.bufferOffset < gob.bufferOffset + 512) ||
                    (gob.x == other.x && gob.y == other.y))
                    return std::nullopt;
            }
        }
        std::vector<vk::BufferImageCopy> copies;
        copies.reserve(gobs.size() * 32);
        for (const auto &gob : gobs)
            for (uint32_t sector{}; sector < 32; ++sector) {
                const uint32_t xBytes{((sector << 3) & 16) | ((sector << 1) & 32)};
                const uint32_t y{((sector >> 1) & 6) | (sector & 1)};
                copies.push_back(vk::BufferImageCopy{
                    .bufferOffset = gob.bufferOffset + sector * 16,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageOffset = {static_cast<int32_t>(gob.x + xBytes / 4), static_cast<int32_t>(gob.y + y), 0},
                    .imageExtent = {4, 1, 1},
                });
            }
        return copies;
    }
}
