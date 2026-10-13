// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace skyline::gpu::texture {
    /** Number of physical rows, counting compressed format blocks rather than pixels. */
    constexpr size_t PitchLinearRowCount(size_t pixelHeight, size_t formatBlockHeight) {
        return pixelHeight / formatBlockHeight + (pixelHeight % formatBlockHeight != 0);
    }

    /** Copy just the visible row data, preserving any destination pitch padding. */
    inline void CopyPitchedRows(const std::uint8_t *source, size_t sourcePitch,
                                std::uint8_t *destination, size_t destinationPitch,
                                size_t rowBytes, size_t rowCount) {
        if (sourcePitch < rowBytes || destinationPitch < rowBytes)
            throw std::invalid_argument("Pitch-linear row is narrower than the format row size");

        for (size_t row{}; row < rowCount; ++row) {
            std::memcpy(destination, source, rowBytes);
            source += sourcePitch;
            destination += destinationPitch;
        }
    }
}
