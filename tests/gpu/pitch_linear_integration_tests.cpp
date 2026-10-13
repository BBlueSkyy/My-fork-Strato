// SPDX-License-Identifier: MPL-2.0
// Compile exact production method bodies against a minimal guest metadata facade.
// Vulkan, allocation and mapping are outside this host test; pitch math/copies are not mocked.
#include <gpu/texture/pitch_linear.h>
#include <array>
#include <cassert>
#include <iostream>
#include <span>

namespace skyline {
    using u8 = std::uint8_t;
    using u32 = std::uint32_t;
    namespace gpu {
        namespace texture {
            enum class TileMode { Linear, Pitch, Block };
            struct Dimensions { u32 width, height, depth; };
            struct Format {
                size_t blockWidth, blockHeight, bpb;
                size_t GetSize(Dimensions d) const {
                    return PitchLinearRowCount(d.width, blockWidth) * PitchLinearRowCount(d.height, blockHeight) * bpb * d.depth;
                }
            };
            // The unrelated block-linear branch is linked but never executed.
            size_t GetBlockLinearLayerSize(Dimensions, size_t, size_t, size_t, size_t, size_t, size_t, bool) {
                throw std::logic_error("Host pitch test entered block-linear path");
            }
        }
        struct GuestTexture {
            texture::Dimensions dimensions{5, 5, 1};
            const texture::Format *format;
            struct {texture::TileMode mode{texture::TileMode::Pitch}; u32 pitch{32}, blockHeight{1}, blockDepth{1};} tileConfig{};
            u32 layerStride{}, layerCount{1}, baseArrayLayer{}, mipLevelCount{1};
            u32 GetLayerStride();
            u32 CalculateLayerSize() const;
            size_t GetSize();
        };
    }
}
#include "pitch-production.inc"

namespace {
    template<typename Exception, typename Function>
    void Rejects(Function function) {
        bool rejected{};
        try { function(); } catch (const Exception &) { rejected = true; }
        assert(rejected);
    }
}

int main() {
    using namespace skyline::gpu;
    const texture::Format bc1{4, 4, 8}, rgba{1, 1, 4};
    GuestTexture guest{.format = &bc1};
    assert(guest.CalculateLayerSize() == 64);
    assert(guest.GetLayerStride() == 64);
    assert(guest.GetSize() == 64);
    std::array<skyline::u8, 64> source{}, result{};
    std::array<skyline::u8, 32> packed{};
    for (size_t i{}; i < source.size(); ++i) source[i] = static_cast<skyline::u8>(i);
    result.fill(0xA5);
    texture::CopyPitchLinearToLinear(guest, source, packed);
    texture::CopyLinearToPitchLinear(guest, packed, result);
    for (size_t i{}; i < result.size(); ++i)
        assert(result[i] == (i % 32 < 16 ? source[i] : 0xA5));
    Rejects<std::out_of_range>([&] { texture::CopyPitchLinearToLinear(guest, std::span{source}.first(47), packed); });
    Rejects<std::out_of_range>([&] { texture::CopyLinearToPitchLinear(guest, std::span{packed}.first(31), result); });
    guest.layerCount = 3;
    guest.baseArrayLayer = 1;
    guest.layerStride = 80;
    assert(guest.GetSize() == 240);
    guest.layerStride = 63;
    Rejects<std::invalid_argument>([&] { (void)guest.GetLayerStride(); });
    guest.layerStride = 0;
    guest.tileConfig.pitch = 15;
    Rejects<std::invalid_argument>([&] { (void)guest.CalculateLayerSize(); });
    guest.tileConfig.pitch = 32;
    guest.dimensions.depth = 2;
    Rejects<std::invalid_argument>([&] { texture::CopyPitchLinearToLinear(guest, source, packed); });
    guest.dimensions.depth = 1;
    guest.mipLevelCount = 2;
    Rejects<std::invalid_argument>([&] { (void)guest.GetSize(); });
    guest.mipLevelCount = 1;
    guest.dimensions = {1, 16384, 1};
    guest.tileConfig.pitch = 0x100000;
    Rejects<std::overflow_error>([&] { (void)guest.CalculateLayerSize(); });
    guest.dimensions = {1, 1, 1};
    guest.tileConfig.pitch = 0x80000000;
    guest.layerCount = 2;
    guest.baseArrayLayer = 0;
    assert(guest.GetSize() == 0x100000000ULL);
    guest = GuestTexture{.dimensions = {7, 5, 1}, .format = &rgba};
    assert(guest.GetSize() == 160);
    assert(guest.CalculateLayerSize() == 160);
    std::cout << "PASS: production GuestTexture size/stride methods and both pitch copy wrappers\n";
}
