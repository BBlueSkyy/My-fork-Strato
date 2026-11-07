// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/line_width_compat.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    using skyline::gpu::interconnect::maxwell3d::ResolveLineWidth;

    const auto defaultWidth{ResolveLineWidth(1.0f, false)};
    assert(!defaultWidth.fallback && defaultWidth.effective == 1.0f);

    const auto supportedWide{ResolveLineWidth(3.0f, true)};
    assert(!supportedWide.fallback && supportedWide.effective == 3.0f);

    for (const float requested : {0.5f, 2.0f, 6.0f}) {
        const auto unsupported{ResolveLineWidth(requested, false)};
        assert(unsupported.fallback && unsupported.effective == 1.0f);
    }

    for (const float requested : {0.0f, -1.0f,
                                  std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(),
                                  -std::numeric_limits<float>::infinity()}) {
        for (const bool supportsWideLines : {false, true}) {
            const auto invalid{ResolveLineWidth(requested, supportsWideLines)};
            assert(invalid.fallback && invalid.effective == 1.0f && std::isfinite(invalid.effective));
        }
    }
    std::puts("PASS: wideLines compatibility fallback");
}
