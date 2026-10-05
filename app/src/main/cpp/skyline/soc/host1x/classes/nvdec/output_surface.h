// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "registers.h"
#include "types.h"

namespace skyline::soc::host1x::nvdec {
    /** @brief Immutable destination of a decode submission; independent of later register writes. */
    struct OutputSurface {
        u32 width{};
        u32 height{};
        u32 lumaPitch{};
        u32 chromaPitch{};
        bool nv24{};
        bool fieldSurfaces{};
        std::array<u64, 2> luma{};
        std::array<u64, 2> chroma{};
    };

    OutputSurface GetH264OutputSurface(const Registers &registers, const H264ParameterSet &params);
}
