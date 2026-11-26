// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <cmath>
#include <gpu/shaders/blit_geometry.h>

int main() {
    using namespace skyline::gpu::blit;
    // Register width32 and backing width17 can share the same guest allocation.
    // A unit copy must still sample each of the17 source texels exactly once.
    auto axis{NormalizeAxis(0, 17, MipDimension(17, 0))};
    for (unsigned x{}; x < 17; ++x) {
        float uv{axis.origin + axis.span * (static_cast<float>(x) + 0.5f) / 17.0f};
        assert(static_cast<unsigned>(std::floor(uv * 17)) == x);
    }
    // Offsets are in texels of the selected mip, not of the parent image.
    auto mipWidth{MipDimension(68, 2)};
    assert(mipWidth == 17);
    auto subrect{NormalizeAxis(3, 7, mipWidth)};
    for (unsigned x{}; x < 7; ++x) {
        float uv{subrect.origin + subrect.span * (static_cast<float>(x) + 0.5f) / 7.0f};
        assert(static_cast<unsigned>(std::floor(uv * mipWidth)) == 3 + x);
    }
    assert(MipDimension(3, 1) == 1);
    assert(MipDimension(1, 4) == 1);
    // Axis normalization preserves a negative span; this does not test the full
    // Fermi slope/origin path.
    auto flipped{NormalizeAxis(10, -4, 17)};
    for (unsigned x{}; x < 4; ++x) {
        float uv{flipped.origin + flipped.span * (static_cast<float>(x) + 0.5f) / 4.0f};
        assert(static_cast<unsigned>(std::floor(uv * 17)) == 9 - x);
    }
}
