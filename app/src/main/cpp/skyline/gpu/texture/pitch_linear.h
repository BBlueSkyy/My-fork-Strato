// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace skyline::gpu::texture {
    /** Number of physical rows, counting compressed format blocks rather than pixels. */
    constexpr size_t PitchLinearRowCount(size_t pixelHeight, size_t formatBlockHeight) {
        if (!formatBlockHeight)
            throw std::invalid_argument("Pitch-linear format block height is zero");
        return pixelHeight / formatBlockHeight + (pixelHeight % formatBlockHeight != 0);
    }

    constexpr size_t PitchLinearCheckedMultiply(size_t value, size_t count) {
        if (count && value > std::numeric_limits<size_t>::max() / count)
            throw std::overflow_error("Pitch-linear size multiplication overflow");
        return value * count;
    }

    struct PitchLinearLayout {
        size_t rowBytes;
        size_t rowCount;
        size_t linearSize;
        std::uint32_t layerSize;
    };

    /** Guest pitch is in bytes; the packed staging row contains only visible blocks. */
    constexpr PitchLinearLayout GetPitchLinearLayout(size_t width, size_t height,
                                                    size_t blockWidth, size_t blockHeight,
                                                    size_t bytesPerBlock, size_t pitch, size_t depth = 1, size_t mipLevelCount = 1) {
        if (depth != 1 || mipLevelCount != 1)
            throw std::invalid_argument("3D or mipmapped pitch-linear textures are unsupported");
        if (!width || !height || !blockWidth || !blockHeight || !bytesPerBlock)
            throw std::invalid_argument("Invalid pitch-linear dimensions or format");
        const auto rowBytes{PitchLinearCheckedMultiply(PitchLinearRowCount(width, blockWidth), bytesPerBlock)};
        const auto rowCount{PitchLinearRowCount(height, blockHeight)};
        if (pitch < rowBytes)
            throw std::invalid_argument("Pitch-linear row is narrower than the format row size");
        const auto layerSize{PitchLinearCheckedMultiply(pitch, rowCount)};
        // GuestTexture's layer stride is u32. Reject instead of silently truncating.
        if (layerSize > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("Pitch-linear layer size exceeds the guest stride range");
        return {rowBytes, rowCount, PitchLinearCheckedMultiply(rowBytes, rowCount), static_cast<std::uint32_t>(layerSize)};
    }

    constexpr size_t PitchLinearTextureSize(size_t layerSize, size_t layerStride,
                                           size_t layerCount, size_t baseArrayLayer) {
        if (!layerSize || !layerCount || baseArrayLayer >= layerCount || layerStride < layerSize)
            throw std::invalid_argument("Invalid pitch-linear layer stride or range");
        // baseArrayLayer selects a view into the backing. Synchronization copies
        // the whole backing, so its mapping must still contain every layer.
        return PitchLinearCheckedMultiply(layerStride, layerCount);
    }

    /** Decode each packed layer separately, retaining its partial block row. */
    template<typename Decoder>
    inline void DecodePitchLinearLayers(std::span<const std::uint8_t> source, size_t sourceLayerSize,
                                         std::span<std::uint8_t> destination, size_t destinationLayerSize,
                                         size_t layerCount, Decoder decode) {
        if (!sourceLayerSize || !destinationLayerSize || !layerCount)
            throw std::invalid_argument("Invalid pitch-linear decode layer size or count");
        const auto sourceSize{PitchLinearCheckedMultiply(sourceLayerSize, layerCount)};
        const auto destinationSize{PitchLinearCheckedMultiply(destinationLayerSize, layerCount)};
        if (source.size() < sourceSize || destination.size() < destinationSize)
            throw std::out_of_range("Pitch-linear decode exceeds the source or destination buffer");
        for (size_t layer{}; layer < layerCount; ++layer)
            decode(source.data() + layer * sourceLayerSize, destination.data() + layer * destinationLayerSize);
    }

    /** Byte extent accessed by the copy, excluding the last row's unused padding. */
    constexpr size_t PitchLinearCopySize(size_t pitch, size_t rowBytes, size_t rowCount) {
        if (!rowCount || !rowBytes)
            return 0;
        const auto lastRow{PitchLinearCheckedMultiply(pitch, rowCount - 1)};
        if (rowBytes > std::numeric_limits<size_t>::max() - lastRow)
            throw std::overflow_error("Pitch-linear copy extent overflow");
        return lastRow + rowBytes;
    }

    /** Copy visible row data only; validate both buffers before the first write. */
    inline void CopyPitchedRows(std::span<const std::uint8_t> source, size_t sourcePitch,
                                std::span<std::uint8_t> destination, size_t destinationPitch,
                                size_t rowBytes, size_t rowCount) {
        if (sourcePitch < rowBytes || destinationPitch < rowBytes)
            throw std::invalid_argument("Pitch-linear row is narrower than the format row size");
        const auto sourceSize{PitchLinearCopySize(sourcePitch, rowBytes, rowCount)};
        const auto destinationSize{PitchLinearCopySize(destinationPitch, rowBytes, rowCount)};
        if (source.size() < sourceSize || destination.size() < destinationSize)
            throw std::out_of_range("Pitch-linear copy exceeds the source or destination buffer");
        if (!rowBytes)
            return;
        // No pointer is advanced beyond the final visible row (the final row
        // need not have pitch padding allocated). All checks are outside the loop.
        for (size_t row{}; row < rowCount; ++row)
            std::memcpy(destination.data() + row * destinationPitch, source.data() + row * sourcePitch, rowBytes);
    }
}
