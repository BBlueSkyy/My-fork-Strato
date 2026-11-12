// SPDX-License-Identifier: MPL-2.0
// The selector is the exact generated production fragment. This fixture only
// supplies candidate metadata; it is not a Vulkan/coherence implementation.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
using u32 = uint32_t;
namespace vk { enum class ImageType { e2D }; }
namespace texture {
    enum class TileMode { Block };
    struct Dims { u32 width{}, height{}, depth{1}; bool operator==(const Dims &) const = default; };
    struct Tile { TileMode mode{TileMode::Block}; size_t blockHeight{16}, blockDepth{1}, tileWidthSpacing{}; bool operator==(const Tile &) const = default; };
    struct Format { size_t blockHeight{1}, blockWidth{1}, bpb{4}; bool IsCompatible(const Format &) const { return true; } };
    struct MipLevelLayout { Dims dimensions; size_t blockHeight, blockDepth, blockLinearSize; };
    static size_t Size(Dims d, size_t bh) { return ((d.width * 4 + 63) / 64) * 64 * ((d.height + bh * 8 - 1) / (bh * 8)) * bh * 8; }
    std::vector<MipLevelLayout> GetBlockLinearMipLayout(Dims d, size_t, size_t, size_t, size_t, size_t, size_t, size_t bh, size_t bd, size_t levels, size_t) {
        if (levels != 1) throw std::runtime_error("fixture expects requested one-mip layout");
        return {{d, bh, bd, Size(d, bh)}};
    }
}
struct Range { u32 baseMipLevel{}, levelCount{1}, baseArrayLayer{}, layerCount{1}; };
struct Guest {
    texture::Dims dimensions{320, 180};
    texture::Tile tileConfig;
    texture::Format storage;
    texture::Format *format{&storage};
    u32 mipLevelCount{1}, viewMipBase{}, viewMipCount{1};
    int viewType{};
    vk::ImageType GetImageType() const { return vk::ImageType::e2D; }
    u32 GetViewLayerCount() const { return 1; }
    size_t GetLayerStride() const { return 438272; }
    size_t CalculateLayerSize() const { return texture::Size(dimensions, tileConfig.blockHeight); }
};
struct Parent {
    std::shared_ptr<Guest> guest{std::make_shared<Guest>()};
    u32 layerCount{1}, levelCount{5};
    bool replaced{};
    std::vector<texture::MipLevelLayout> mipLayouts;
    bool ViewRangeFits(int, Range r) const { return r.baseMipLevel < levelCount && r.levelCount <= levelCount - r.baseMipLevel && r.baseArrayLayer < layerCount; }
};
struct Mapping { std::shared_ptr<Parent> texture; };
struct Match { size_t offset; };
static int Select(Guest &guestTexture, std::shared_ptr<Parent> parent, size_t offset) {
    auto &matchGuestTexture{*parent->guest};
    Mapping mapping{parent}; auto *hostMapping{&mapping};
    Match match{offset}; auto *mappingMatch{&match};
    std::shared_ptr<Parent> layerMipMatch{}, fullMatch{};
    u32 matchLayer{}, matchLevel{};
    Range requestedRange;
    const auto requestedStride{guestTexture.CalculateLayerSize()};
#include "selector.inc"
    (void)matchLayer;
    return layerMipMatch ? static_cast<int>(matchLevel) : -1;
}
static void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
    auto parent{std::make_shared<Parent>()};
    const u32 widths[]{320,160,80,40,20}, heights[]{180,90,45,22,11};
    const size_t gobs[]{16,16,8,4,2};
    size_t offsets[5]{}, total{};
    for (size_t i{}; i < 5; ++i) {
        offsets[i] = total;
        const texture::Dims d{widths[i], heights[i]};
        const auto bytes{texture::Size(d, gobs[i])};
        parent->mipLayouts.push_back({d, gobs[i], 1, bytes}); total += bytes;
    }
    Check(total == 438272, "captured bloom physical footprint mismatch");
    for (size_t i{}; i < 5; ++i) {
        Guest request;
        request.dimensions = {widths[i], heights[i]}; request.tileConfig.blockHeight = gobs[i];
        const int selected{Select(request, parent, offsets[i])};
        const int expected{POLICY == 2 && i > 0 ? -1 : POLICY == 1 && i > 1 ? -1 : static_cast<int>(i)};
        Check(selected == expected, "variant admitted the wrong mip");
        Check(Select(request, parent, offsets[i] + 512) == -1, "non-boundary offset accepted");
    }
    Guest bad;
    bad.dimensions = {81,45}; bad.tileConfig.blockHeight = 8;
    Check(Select(bad, parent, offsets[2]) == -1, "wrong dimensions with equal allocation size accepted");
    Guest wrongGob;
    wrongGob.dimensions = {80,45}; wrongGob.tileConfig.blockHeight = 4;
    Check(Select(wrongGob, parent, offsets[2]) == -1, "wrong physical GOB layout accepted");
    std::cout << "PASS policy=" << POLICY << ": five bloom mips, exact offsets, dimensions and GOB rejection\n";
}
