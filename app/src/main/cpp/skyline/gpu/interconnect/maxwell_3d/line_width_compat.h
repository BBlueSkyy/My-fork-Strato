// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors
#pragma once

#include <cmath>

namespace skyline::gpu::interconnect::maxwell3d {
    struct LineWidthCompatibility {
        float effective;
        bool fallback;
    };

    // Vulkan requires lineWidth == 1.0 when the wideLines feature is disabled.
    // Reject invalid widths even on supported hosts; do not preserve a stale
    // dynamic line-width state when the guest requests an unsupported value.
    inline LineWidthCompatibility ResolveLineWidth(float requested, bool supportsWideLines) {
        if (!std::isfinite(requested) || requested <= 0.0f ||
            (!supportsWideLines && requested != 1.0f))
            return {1.0f, true};
        return {requested, false};
    }
}
