// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "texture.h"

namespace skyline::gpu::texture {
    /**
     * @brief Describes how two guest texture formats can share a host representation
     *
     * CopyCompatible is intentionally not produced yet. It is reserved for formats that
     * require an explicit conversion/copy path rather than sharing the same host image.
     */
    enum class Compatibility : u8 {
        Exact,
        ViewCompatible,
        CopyCompatible,
        Incompatible,
    };

    /**
     * @brief Classifies format compatibility without making resource lifetime decisions
     *
     * This preserves the existing TextureManager semantics while separating format
     * compatibility from cache/aliasing policy.
     */
    constexpr Compatibility ClassifyCompatibility(const FormatBase &lhs, const FormatBase &rhs) {
        if (lhs == rhs)
            return Compatibility::Exact;

        if (lhs.IsCompatible(rhs))
            return Compatibility::ViewCompatible;

        return Compatibility::Incompatible;
    }

    /**
     * @return Whether both formats can currently share the same host image storage
     */
    constexpr bool CanShareStorage(Compatibility compatibility) {
        return compatibility == Compatibility::Exact || compatibility == Compatibility::ViewCompatible;
    }
}
