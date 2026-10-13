// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/pitch_linear.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

int main() {
    using namespace skyline::gpu::texture;
    assert(PitchLinearRowCount(128, 4) == 32);
    assert(PitchLinearRowCount(5, 4) == 2);
    assert(PitchLinearRowCount(1, 4) == 1);
    assert(PitchLinearRowCount(128, 1) == 128);
    assert(PitchLinearRowCount(0, 4) == 0);

    // BC3: 128 pixels wide, 16 bytes per 4x4 block, padded from 512 to 528 bytes per row.
    constexpr size_t rowBytes{(128 / 4) * 16}, rowPitch{rowBytes + 16};
    constexpr size_t rows{PitchLinearRowCount(128, 4)};
    std::vector<std::uint8_t> guest(rowPitch * rows, 0xEA), linear(rowBytes * rows, 0xCC);
    for (size_t row{}; row < rows; ++row)
        for (size_t col{}; col < rowBytes; ++col)
            guest[row * rowPitch + col] = static_cast<std::uint8_t>((row * 13 + col) % 251);
    CopyPitchedRows(guest.data(), rowPitch, linear.data(), rowBytes, rowBytes, rows);
    for (size_t row{}; row < rows; ++row)
        for (size_t col{}; col < rowBytes; ++col)
            assert(linear[row * rowBytes + col] == guest[row * rowPitch + col]);

    // Upload back into a separate pitched guest allocation. Preserve pitch padding.
    std::vector<std::uint8_t> roundtrip(rowPitch * rows, 0xA5);
    CopyPitchedRows(linear.data(), rowBytes, roundtrip.data(), rowPitch, rowBytes, rows);
    for (size_t row{}; row < rows; ++row) {
        for (size_t col{}; col < rowBytes; ++col)
            assert(roundtrip[row * rowPitch + col] == guest[row * rowPitch + col]);
        for (size_t col{rowBytes}; col < rowPitch; ++col)
            assert(roundtrip[row * rowPitch + col] == 0xA5);
    }

    // Odd-sized compressed surface (5x5 BC1): two rows of two blocks.
    std::array<std::uint8_t, 40> oddPitch{};
    std::array<std::uint8_t, 32> oddLinear{};
    for (size_t i{}; i < oddPitch.size(); ++i)
        oddPitch[i] = static_cast<std::uint8_t>(i);
    CopyPitchedRows(oddPitch.data(), 20, oddLinear.data(), 16, 16, PitchLinearRowCount(5, 4));
    for (size_t col{}; col < 16; ++col) {
        assert(oddLinear[col] == oddPitch[col]);
        assert(oddLinear[16 + col] == oddPitch[20 + col]);
    }

    // Uncompressed 4-byte pixels still require one copy per pixel row.
    std::array<std::uint8_t, 80> rgbaGuest{};
    std::array<std::uint8_t, 64> rgbaLinear{};
    for (size_t i{}; i < rgbaGuest.size(); ++i) rgbaGuest[i] = static_cast<std::uint8_t>(i);
    CopyPitchedRows(rgbaGuest.data(), 20, rgbaLinear.data(), 16, 16, PitchLinearRowCount(4, 1));
    for (size_t row{}; row < 4; ++row)
        for (size_t col{}; col < 16; ++col)
            assert(rgbaLinear[row * 16 + col] == rgbaGuest[row * 20 + col]);

    // Reject a physically impossible pitch instead of overlapping row memory.
    bool rejected{};
    try {
        CopyPitchedRows(guest.data(), rowBytes - 1, linear.data(), rowBytes, rowBytes, rows);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
