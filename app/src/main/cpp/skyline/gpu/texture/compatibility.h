// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "resource_compatibility.h"
#include "texture.h"

namespace skyline::gpu::texture {
    /**
     * @brief Classifies format compatibility without making resource lifetime decisions
     *
     * This preserves the existing TextureManager semantics while separating format
     * compatibility from cache/aliasing policy.
     */
    constexpr FormatCompatibility ClassifyFormatCompatibility(const FormatBase &lhs, const FormatBase &rhs) {
        if (lhs == rhs)
            return FormatCompatibility::Exact;

        if (lhs.IsCompatible(rhs))
            return FormatCompatibility::ViewCompatible;

        return FormatCompatibility::Incompatible;
    }

    /**
     * @return Whether both formats can currently share the same host image storage
     */
    constexpr bool CanShareStorage(FormatCompatibility compatibility) {
        return compatibility == FormatCompatibility::Exact || compatibility == FormatCompatibility::ViewCompatible;
    }
}
