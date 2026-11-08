// SPDX-License-Identifier: MPL-2.0
// Compile the production CopyToGuest body with small GPU/texture facades.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
template<class... T> std::runtime_error exception(const char *, T...) {
    return std::runtime_error("unsupported mip/tiling");
}
namespace texture {
    enum class TileMode { Linear, Pitch, Block };
    struct Dimensions { u32 width{}, height{}, depth{}; };
    struct Format { u32 blockWidth{}, blockHeight{}, bpb{}; };
    struct TileConfig { TileMode mode{TileMode::Linear}; };
    template<class... T> void CopyLinearToBlockLinear(T &&...) {}
}
struct GuestTexture {
    texture::TileConfig tileConfig;
    texture::Format *format{};
    std::size_t layerStride{};
    std::size_t GetSize() const { return layerStride; }
    std::size_t GetLayerStride() const { return layerStride; }
};
namespace texture {
    void CopyLinearToPitchLinear(const GuestTexture &, std::span<u8>, std::span<u8>) {}
}
struct Level {
    texture::Dimensions dimensions;
    std::size_t linearSize{};
    u32 blockHeight{}, blockDepth{}, tileWidthSpacing{};
};
struct Texture {
    GuestTexture *guest{};
    std::span<u8> mirror;
    std::atomic<bool> regionTracked{false};
    std::size_t layerStride{}, layerCount{};
    u32 levelCount{1};
    int tiling{};
    std::vector<Level> mipLayouts;
    bool selectivePublish{false};
    void PublishRegionGuest(std::span<const u8> encoded) {
        // Simulate selective ownership: an unowned later layer must be retained.
        const auto size = selectivePublish ? layerStride : encoded.size();
        std::copy_n(encoded.begin(), size, mirror.begin());
    }
    void CopyToGuest(u8 *hostBuffer);
};
#include "production.inc"

void Run(bool tracked, bool selective, std::size_t layers) {
    constexpr std::size_t stride = 16;
    std::vector<u8> source(stride * layers), destination(stride * layers, 0xCC);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<u8>(i + 1);
    const auto originalSource = source;
    texture::Format fmt{1, 1, 1};
    GuestTexture guest{{texture::TileMode::Linear}, &fmt, stride};
    Texture texture;
    texture.guest = &guest;
    texture.mirror = destination;
    texture.layerStride = stride;
    texture.layerCount = layers;
    texture.regionTracked = tracked;
    texture.selectivePublish = selective;
    texture.CopyToGuest(source.data());
    assert(source == originalSource && "GPU readback source was overwritten");
    for (std::size_t i = 0; i < destination.size(); ++i) {
        const u8 expected = selective && i >= stride ? static_cast<u8>(0xCC) : originalSource[i];
        assert(destination[i] == expected && "readback not published into guest-owned bytes");
    }
}
int main() {
    Run(false, false, 1);
    Run(false, false, 3);
    Run(true, false, 1);
    Run(true, false, 3);
    Run(true, true, 3);
    std::cout << "PASS: GPU readback ownership, linear layers and selective region publishing\n";
}
