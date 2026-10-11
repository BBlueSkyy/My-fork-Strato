// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "copy_format_compatibility.h"
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
        return ClassifyHostFormatCompatibility(
            lhs.vkFormat, lhs.vkAspect, rhs.vkFormat, rhs.vkAspect,
            lhs.IsCompatible(rhs));
    }

    /**
     * @return Whether both formats can currently share the same host image storage
     */
    constexpr bool CanShareStorage(FormatCompatibility compatibility) {
        return compatibility == FormatCompatibility::Exact || compatibility == FormatCompatibility::ViewCompatible;
    }
}
