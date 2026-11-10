// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/common/botw_pixel_probe.h"
#include <array>
#include <cassert>
#include <cstdint>

int main() {
    using namespace skyline::diag;
    static_assert(BotwPixelProbeBytes == 4096);
    static_assert(BotwPixelPatchBytes == 1024);
    constexpr auto positions{BotwPixelRects(1920, 1080)};
    static_assert(positions[0].x == 480 && positions[0].y == 270);
    static_assert(positions[1].x == 1440 && positions[1].y == 270);
    static_assert(positions[2].x == 480 && positions[2].y == 810);
    static_assert(positions[3].x == 1440 && positions[3].y == 810);

    std::array<std::uint8_t, BotwPixelProbeBytes> image{};
    const auto original{HashBotwPixelPatches(image.data())};
    assert(ChangedBotwPixelPatches(original, original) == 0);

    image[7] = 5;
    const auto changed{HashBotwPixelPatches(image.data())};
    assert(ChangedBotwPixelPatches(original, changed) == 1);
    assert(original[0] != changed[0]);
    for (std::size_t i = 1; i < BotwPixelPatchCount; ++i)
        assert(original[i] == changed[i]);

    image[3 * BotwPixelPatchBytes + 15] = 200;
    const auto changedAgain{HashBotwPixelPatches(image.data())};
    assert(ChangedBotwPixelPatches(original, changedAgain) == 2);
    assert(ChangedBotwPixelPatches(changed, changedAgain) == 1);
}
