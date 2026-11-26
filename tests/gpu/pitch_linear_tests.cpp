// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/pitch_linear.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

using namespace skyline::gpu::texture;

namespace {
    template<typename Exception, typename Function>
    void Rejects(Function function) {
        bool rejected{};
        try { function(); } catch (const Exception &) { rejected = true; }
        assert(rejected);
    }

    // Independent pixel/block oracle. Source rows, destination padding and layer
    // gaps all have distinct patterns, so a wrong stride cannot pass a round-trip.
    void RoundTrip(size_t width, size_t height, size_t blockWidth, size_t blockHeight,
                   size_t bytesPerBlock, size_t padding, size_t layers) {
        const size_t columns{(width + blockWidth - 1) / blockWidth};
        const size_t rows{(height + blockHeight - 1) / blockHeight};
        const size_t rowBytes{columns * bytesPerBlock};
        const size_t pitch{rowBytes + padding};
        const auto layout{GetPitchLinearLayout(width, height, blockWidth, blockHeight, bytesPerBlock, pitch)};
        assert(layout.rowBytes == rowBytes && layout.rowCount == rows);
        assert(layout.layerSize == pitch * rows && layout.linearSize == rowBytes * rows);
        const size_t guestStride{layout.layerSize + 37};
        std::vector<std::uint8_t> guest(guestStride * layers, 0xEA);
        std::vector<std::uint8_t> linear(layout.linearSize * layers, 0xCC);
        std::vector<std::uint8_t> roundtrip(guest.size(), 0xA5);
        for (size_t layer{}; layer < layers; ++layer) {
            for (size_t row{}; row < rows; ++row)
                for (size_t col{}; col < rowBytes; ++col)
                    guest[layer * guestStride + row * pitch + col] = static_cast<std::uint8_t>((layer * 71 + row * 13 + col) % 251);
            auto packed{std::span{linear}.subspan(layer * layout.linearSize, layout.linearSize)};
            CopyPitchedRows(std::span{guest}.subspan(layer * guestStride, layout.layerSize), pitch,
                           packed, rowBytes, rowBytes, rows);
            for (size_t row{}; row < rows; ++row)
                for (size_t col{}; col < rowBytes; ++col)
                    assert(packed[row * rowBytes + col] == guest[layer * guestStride + row * pitch + col]);
            CopyPitchedRows(packed, rowBytes,
                           std::span{roundtrip}.subspan(layer * guestStride, layout.layerSize), pitch, rowBytes, rows);
        }
        for (size_t layer{}; layer < layers; ++layer) {
            for (size_t offset{}; offset < guestStride; ++offset) {
                const bool visible{offset < layout.layerSize && offset % pitch < rowBytes};
                assert(roundtrip[layer * guestStride + offset] == (visible ? guest[layer * guestStride + offset] : 0xA5));
            }
        }
    }
}

int main() {
    static_assert(PitchLinearRowCount(128, 4) == 32);
    static_assert(PitchLinearRowCount(5, 4) == 2);
    static_assert(PitchLinearRowCount(1, 4) == 1);
    static_assert(PitchLinearRowCount(128, 1) == 128);
    static_assert(PitchLinearRowCount(0, 4) == 0);
    assert(PitchLinearRowCount(std::numeric_limits<size_t>::max(), 4) == std::numeric_limits<size_t>::max() / 4 + 1);
    Rejects<std::invalid_argument>([] { (void)PitchLinearRowCount(1, 0); });

    size_t cases{};
    // BC1/4 = 8 bytes; BC2/3/5/6/7 = 16 bytes, including UNORM,
    // SNORM and sRGB variants (their physical layout is identical).
    for (size_t bytes : {8U, 16U})
        for (auto extent : {std::array<size_t, 2>{128, 128}, {5, 5}, {1, 1}, {7, 9}, {4, 4}})
            for (size_t padding : {0U, 1U, 16U, 4096U})
                for (size_t layers : {1U, 3U}) {
                    RoundTrip(extent[0], extent[1], 4, 4, bytes, padding, layers);
                    ++cases;
                }
    // Every ASTC 2D block footprint, including asymmetric blocks.
    for (auto block : {std::array<size_t, 2>{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6},
                       {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}})
        for (size_t padding : {0U, 1U, 128U, 4096U})
            for (size_t layers : {1U, 3U}) {
                RoundTrip(block[0] * 3, block[1] * 2, block[0], block[1], 16, padding, layers);
                RoundTrip(block[0] * 3 + 1, block[1] * 2 + 1, block[0], block[1], 16, padding, layers);
                cases += 2;
            }
    // RGBA and BGRA: identical byte layout, no component conversion.
    for (size_t padding : {0U, 1U, 16U, 4096U}) {
        RoundTrip(7, 5, 1, 1, 4, padding, 3);
        ++cases;
    }
    RoundTrip(16384, 3, 1, 1, 16, 32, 1); // wide 128-bit pixels
    RoundTrip(5, 16384, 4, 4, 8, 32, 1); // tall compressed image
    cases += 2;

    std::array<std::uint8_t, 40> guest{};
    std::array<std::uint8_t, 32> linear{};
    linear.fill(0xCC);
    const auto unchanged{linear};
    Rejects<std::invalid_argument>([&] { CopyPitchedRows(guest, 15, linear, 16, 16, 2); });
    Rejects<std::invalid_argument>([&] { CopyPitchedRows(guest, 20, linear, 15, 16, 2); });
    // One-byte-short source/destination must fail before copying even row zero.
    Rejects<std::out_of_range>([&] { CopyPitchedRows(std::span{guest}.first(35), 20, linear, 16, 16, 2); });
    Rejects<std::out_of_range>([&] { CopyPitchedRows(guest, 20, std::span{linear}.first(31), 16, 16, 2); });
    assert(linear == unchanged);
    // Last-row padding is not accessed or required by the copy.
    CopyPitchedRows(std::span{guest}.first(36), 20, linear, 16, 16, 2);
    CopyPitchedRows({}, 0, {}, 0, 0, 0);
    CopyPitchedRows({}, 0, {}, 0, 0, 100);

    constexpr auto max{std::numeric_limits<size_t>::max()};
    Rejects<std::overflow_error>([&] { CopyPitchedRows(guest, max, linear, 16, 16, 2); });
    Rejects<std::overflow_error>([&] { CopyPitchedRows(guest, 20, linear, max, 16, 2); });
    Rejects<std::overflow_error>([] { (void)GetPitchLinearLayout(max, 1, 1, 1, 16, max); });
    Rejects<std::overflow_error>([] { (void)GetPitchLinearLayout(1, max, 1, 1, 1, 2); });
    // Existing GuestTexture API stores layer strides in u32. Never narrow 4 GiB to 0.
    Rejects<std::overflow_error>([] { (void)GetPitchLinearLayout(1, 16384, 4, 4, 8, 0x100000); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(5, 5, 4, 4, 8, 15); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(0, 5, 4, 4, 8, 32); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(5, 0, 4, 4, 8, 32); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(5, 5, 0, 4, 8, 32); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(5, 5, 4, 0, 8, 32); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(5, 5, 4, 4, 0, 32); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(5, 5, 4, 4, 8, 32, 2, 1); });
    Rejects<std::invalid_argument>([] { (void)GetPitchLinearLayout(5, 5, 4, 4, 8, 32, 1, 2); });
    const auto large{GetPitchLinearLayout(16384, 16384, 1, 1, 4, 65536)};
    assert(large.layerSize == 1073741824 && large.linearSize == 1073741824);
    assert(PitchLinearTextureSize(64, 80, 3, 0) == 240);
    // Base layer selects a view; the backing uploader still copies all layers.
    assert(PitchLinearTextureSize(64, 80, 3, 1) == 240);
    assert(PitchLinearTextureSize(0x80000000U, 0x80000000U, 2, 0) == 0x100000000ULL);
    Rejects<std::invalid_argument>([] { (void)PitchLinearTextureSize(64, 63, 3, 0); });
    Rejects<std::invalid_argument>([] { (void)PitchLinearTextureSize(64, 80, 0, 0); });
    Rejects<std::invalid_argument>([] { (void)PitchLinearTextureSize(64, 80, 3, 3); });
    Rejects<std::overflow_error>([] { (void)PitchLinearTextureSize(1, max, 2, 0); });
    // Odd-height BC layers cannot be decoded as one tall image: the last
    // partial block row of one layer must never become pixels in the next.
    std::array<std::uint8_t, 64> compressed{}; // 2 * (2x2 BC1 blocks)
    std::array<std::uint8_t, 200> decoded{}; // 2 * (5x5 RGBA)
    compressed[0] = 11;
    compressed[32] = 97;
    size_t decodedLayers{};
    DecodePitchLinearLayers(compressed, 32, decoded, 100, 2,
        [&](const std::uint8_t *input, std::uint8_t *output) {
            std::fill_n(output, 100, input[0]);
            ++decodedLayers;
        });
    assert(decodedLayers == 2);
    for (size_t i{}; i < decoded.size(); ++i)
        assert(decoded[i] == (i < 100 ? 11 : 97));
    decodedLayers = 0;
    Rejects<std::out_of_range>([&] {
        DecodePitchLinearLayers(std::span{compressed}.first(63), 32, decoded, 100, 2,
            [&](auto, auto) { ++decodedLayers; });
    });
    Rejects<std::out_of_range>([&] {
        DecodePitchLinearLayers(compressed, 32, std::span{decoded}.first(199), 100, 2,
            [&](auto, auto) { ++decodedLayers; });
    });
    assert(decodedLayers == 0);
    std::cout << "PASS: " << cases << " layout round-trips plus bounds/overflow regressions\n";
}
