// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>

namespace skyline::soc::host1x {
    struct SurfacePlane {
        u64 address;
        u32 widthBytes;
        u32 height;
        u32 pitch;
        bool blockLinear;
        u32 blockHeightLog2;
    };

    size_t GetSurfacePlaneSize(const SurfacePlane &plane);

    /** @brief Validates dimensions, SMMU address range and mappings before any plane is written. */
    void ValidateSurfacePlane(const DeviceState &state, const SurfacePlane &plane);

    /** @brief Writes a pitch-strided plane, using the shared GPU layout helpers for block-linear surfaces. */
    void WriteSurfacePlane(const DeviceState &state, const SurfacePlane &plane, span<u8> linear);
}
