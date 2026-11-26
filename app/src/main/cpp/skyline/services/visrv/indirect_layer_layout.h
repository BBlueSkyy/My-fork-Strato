// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <cstdint>
#include <limits>

namespace skyline::service::visrv {
    constexpr std::uint64_t IndirectLayerAlignment{0x1000};
    constexpr std::uint64_t IndirectLayerRequiredSizeAlignment{0x20000};

    struct IndirectLayerLayout {
        std::uint64_t stride{};
        std::uint64_t imageSize{};
        std::uint64_t requiredSize{};
    };

    constexpr std::uint64_t AlignIndirectLayerSize(std::uint64_t value, std::uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    inline bool CalculateIndirectLayerLayout(std::int64_t width, std::int64_t height, IndirectLayerLayout &layout) {
        constexpr std::uint64_t MaxSize{std::numeric_limits<std::int64_t>::max()};
        if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) > MaxSize / 4)
            return false;

        layout.stride = static_cast<std::uint64_t>(width) * 4;
        if (static_cast<std::uint64_t>(height) > MaxSize / layout.stride)
            return false;

        layout.imageSize = layout.stride * static_cast<std::uint64_t>(height);
        if (layout.imageSize > MaxSize - (IndirectLayerRequiredSizeAlignment - 1))
            return false;
        layout.requiredSize = AlignIndirectLayerSize(layout.imageSize, IndirectLayerRequiredSizeAlignment);
        return true;
    }
}
