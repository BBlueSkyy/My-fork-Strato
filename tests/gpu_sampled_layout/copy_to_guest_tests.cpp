// SPDX-License-Identifier: MPL-2.0
// Exact production CopyToGuest and tiling helpers; no Vulkan I/O is involved.
#include "host_types.h"
#include <gpu/texture/overlap_group.h>
namespace skyline::gpu {
    class Texture {
      public:
        std::optional<GuestTexture> guest;
        std::span<u8> mirror;
        u32 levelCount, layerCount;
        size_t layerStride;
        std::vector<texture::MipLevelLayout> mipLayouts;
        vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
        bool HasOverlaps() const { return false; } // Native no-overlap encoder regression.
        std::shared_ptr<texture::OverlapState> overlapState{};
        void CopyToGuest(u8 *hostBuffer);
        void EncodeGuest(u8 *,u8 *, GuestTexture * = nullptr, const std::vector<texture::MipLevelLayout> * = nullptr);
    };
#include "copy-to-guest.inc"
}
namespace {
    using namespace skyline;
    using namespace skyline::gpu;
    void Check(bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    }
    // Independent byte addressing within one Maxwell GOB (64 bytes x 8 rows).
    size_t GobOffset(size_t x, size_t y) {
        return ((x & 32) << 3) + ((y & 6) << 5) + ((x & 16) << 1) +
               ((y & 1) << 4) + (x & 15);
    }
    void CopyCase(texture::TileMode mode, u32 layers, bool mipmapped = false) {
        constexpr size_t Guard{16};
        constexpr u8 GuestSentinel{0xE7}, SourceSentinel{0xC5};
        const texture::Format rgba{1, 1, 4};
        const bool block{mode == texture::TileMode::Block};
        const u32 width{block ? 8U : 3U}, height{mipmapped ? 8U : 2U};
        const size_t packedLayer{size_t{width} * height * 4};
        const u32 guestStride{block ? 1024U : mode == texture::TileMode::Pitch ? 48U : 32U};
        GuestTexture guest{
            .dimensions = {width, height, 1}, .format = &rgba,
            .tileConfig = {.mode = mode, .blockHeight = 1, .blockDepth = 1, .tileWidthSpacing = 0},
            .viewType = vk::ImageViewType::e2DArray, .layerStride = guestStride,
            .layerCount = layers, .mipLevelCount = mipmapped ? 2U : 1U,
        };
        if (mode == texture::TileMode::Pitch) guest.tileConfig.pitch = 20;
        const size_t sourceSize{(packedLayer + (mipmapped ? 64 : 0)) * layers};
        std::vector<u8> source(Guard + sourceSize + Guard, SourceSentinel);
        for (size_t i{}; i < sourceSize; ++i) source[Guard + i] = static_cast<u8>(1 + i % 191);
        const auto sourceBefore{source};
        std::vector<u8> destination(Guard + size_t{guestStride} * layers + Guard, GuestSentinel);
        auto expected{destination};
        for (u32 layer{}; layer < layers; ++layer) {
            size_t mipSourceBase{}, mipGuestBase{};
            for (u32 mip{}; mip < guest.mipLevelCount; ++mip) {
                const size_t rowBytes{(width >> mip) * 4}, rows{height >> mip};
                for (size_t y{}; y < rows; ++y) for (size_t x{}; x < rowBytes; ++x) {
                    const size_t offset{block ? GobOffset(x, y) :
                        mode == texture::TileMode::Pitch ? y * 20 + x : y * rowBytes + x};
                    expected[Guard + layer * guestStride + mipGuestBase + offset] =
                        sourceBefore[Guard + mipSourceBase + layer * rowBytes * rows + y * rowBytes + x];
                }
                mipSourceBase += rowBytes * rows * layers;
                mipGuestBase += 512;
            }
        }
        Texture texture{.guest = guest,
            .mirror = {destination.data() + Guard, size_t{guestStride} * layers},
            .levelCount = guest.mipLevelCount, .layerCount = layers, .layerStride = packedLayer,
            .mipLayouts = texture::GetBlockLinearMipLayout(guest.dimensions,
                1, 1, 4, 1, 1, 4, 1, 1, guest.mipLevelCount, 0),
        };
        texture.CopyToGuest(source.data() + Guard);
        Check(destination == expected, "CopyToGuest did not write GPU bytes to the correct guest layer/row or changed padding/guards");
        Check(source == sourceBefore, "CopyToGuest modified the GPU/staging source");
    }
}
int main() {
    try {
        for (auto mode : {texture::TileMode::Linear, texture::TileMode::Pitch, texture::TileMode::Block})
            for (u32 layers : {1U, 3U}) CopyCase(mode, layers);
        CopyCase(texture::TileMode::Block, 3, true);
        std::cout << "PASS: production CopyToGuest Linear/Pitch/Block, 1/3 layers, block mips, source immutability and guest padding/guards\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
