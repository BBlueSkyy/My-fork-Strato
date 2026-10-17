// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <gpu/texture/depth_stencil.h>
#include <gpu/texture/pitch_linear.h>
using u8 = uint8_t;
using u32 = uint32_t;
template<typename... A> std::runtime_error exception(const char *text, A &&...) { return std::runtime_error(text); }
namespace bcn {
template<typename... A> void DecodeBc1(A &&...) { throw std::runtime_error("Unused BC decoder"); }
template<typename... A> void DecodeBc2(A &&...) { throw std::runtime_error("Unused BC decoder"); }
template<typename... A> void DecodeBc3(A &&...) { throw std::runtime_error("Unused BC decoder"); }
template<typename... A> void DecodeBc4(A &&...) { throw std::runtime_error("Unused BC decoder"); }
template<typename... A> void DecodeBc5(A &&...) { throw std::runtime_error("Unused BC decoder"); }
template<typename... A> void DecodeBc6(A &&...) { throw std::runtime_error("Unused BC decoder"); }
template<typename... A> void DecodeBc7(A &&...) { throw std::runtime_error("Unused BC decoder"); }
}
namespace skyline::gpu {
struct Format { vk::Format vkFormat; vk::ImageAspectFlags vkAspect; size_t bpb; bool stencilFirst{}; size_t blockWidth{1}, blockHeight{1}; };
namespace texture {
enum class TileMode { Block, Pitch, Linear };
struct Level { vk::Extent3D dimensions; size_t linearSize, targetLinearSize, blockLinearSize{}, blockHeight{1}, blockDepth{1}; };
}
struct GuestTexture {
    vk::Extent3D dimensions;
    Format *format;
    struct { texture::TileMode mode; size_t pitch; } tileConfig;
    size_t mipLevelCount{1}, stride{}, layers{};
    size_t GetLayerStride() const { return stride; }
    size_t GetSize() const { return stride * layers; }
};
namespace texture {
template<typename... A> void CopyBlockLinearToLinear(A &&...) { throw std::runtime_error("Unused block copy"); }
template<typename... A> void CopyLinearToBlockLinear(A &&...) { throw std::runtime_error("Unused block copy"); }
#include "pitch.inc"
}
namespace memory {
struct StagingBuffer {
    std::vector<u8> bytes;
    explicit StagingBuffer(size_t size) : bytes(size, 0xDA) {}
    u8 *data() { return bytes.data(); }
    size_t size() const { return bytes.size(); }
};
struct Image { u8 *data() { throw std::runtime_error("Unused mapped Vulkan image"); } };
}
struct GPU { struct { std::shared_ptr<memory::StagingBuffer> AllocateStagingBuffer(size_t size) { return std::make_shared<memory::StagingBuffer>(size); } } memory; };
struct Texture {
    GPU gpu;
    GuestTexture *guest{};
    vk::Extent3D dimensions{};
    Format *format{};
    vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
    vk::ImageLayout layout{vk::ImageLayout::eUndefined};
    std::variant<memory::Image, int> backing{0};
    std::vector<u8> mirror;
    std::vector<texture::Level> mipLayouts;
    size_t surfaceSize{}, deswizzledSurfaceSize{}, deswizzledLayerStride{}, layerStride{}, layerCount{}, levelCount{1};
    void WaitOnBacking() {}
    void WaitOnFence() { throw std::runtime_error("Unused mapped image fence"); }
    void TransitionLayout(vk::ImageLayout) { throw std::runtime_error("Unused mapped image transition"); }
    std::shared_ptr<memory::StagingBuffer> SynchronizeHostImpl();
    void CopyToGuest(u8 *);
};
#include "production.inc"

static u32 Load(const u8 *p) { u32 value; std::memcpy(&value, p, 4); return value; }
static void Store(u8 *p, u32 value) { std::memcpy(p, &value, 4); }
static std::vector<u8> ReferenceTransfer(const std::vector<u8> &packed, const Format &format) {
    bool combined{format.vkAspect == (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil)};
    if (!combined) {
        auto result{packed};
        result.resize((packed.size() + 3) & ~size_t{3}, 0);
        return result;
    }
    size_t texels{packed.size() / format.bpb}, stencilOffset{(packed.size() + 3) & ~size_t{3}};
    std::vector<u8> result(stencilOffset + ((texels + 3) & ~size_t{3}));
    for (size_t i{}; i < texels; ++i) {
        u32 value{Load(packed.data() + i * format.bpb)};
        u8 stencil{};
        if (format.bpb == 8) stencil = packed[i * 8 + 4];
        else if (format.stencilFirst) { stencil = static_cast<u8>(value); value >>= 8; }
        else { stencil = value >> 24; value &= 0xffffff; }
        Store(result.data() + i * 4, value);
        result[stencilOffset + i] = stencil;
    }
    return result;
}

static void Check(Format format, size_t layers, texture::TileMode mode, const std::string &only) {
    size_t rowBytes{3 * format.bpb}, linearLayer{rowBytes * 3};
    size_t pitch{rowBytes + (mode == texture::TileMode::Pitch ? 7 : 0)};
    GuestTexture guest{{3, 3, 1}, &format, {mode, pitch}, 1, pitch * 3 + 11, layers};
    Texture texture{};
    texture.guest = &guest;
    texture.dimensions = guest.dimensions;
    texture.format = &format;
    texture.layerCount = layers;
    texture.layerStride = texture.deswizzledLayerStride = linearLayer;
    texture.surfaceSize = texture.deswizzledSurfaceSize = linearLayer * layers;
    texture.mipLayouts.push_back({guest.dimensions, linearLayer, linearLayer});
    texture.mirror.resize(guest.GetSize(), 0xE3);
    std::vector<u8> packed(texture.surfaceSize);
    for (size_t i{}; i < packed.size(); ++i) packed[i] = static_cast<u8>(i * 29 + 5);
    if (format.bpb == 8) for (size_t i{}; i < packed.size(); i += 8) {
        float depth{static_cast<float>((i / 8) % 13) / 16.0f};
        std::memcpy(packed.data() + i, &depth, 4);
        packed[i + 5] = packed[i + 6] = packed[i + 7] = 0;
    }
    auto putGuest = [&](const std::vector<u8> &data) {
        for (size_t layer{}; layer < layers; ++layer)
            for (size_t row{}; row < 3; ++row)
                std::memcpy(texture.mirror.data() + layer * guest.stride + row * pitch,
                    data.data() + layer * linearLayer + row * rowBytes, rowBytes);
    };
    putGuest(packed);
    if (only != "readback") {
        auto staging{texture.SynchronizeHostImpl()};
        auto reference{ReferenceTransfer(packed, format)};
        assert(staging && staging->size() == reference.size());
        // Only meaningful transfer bytes are checked; aligned padding is unspecified.
        bool combined{format.vkAspect == (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil)};
        size_t depthBytes{combined ? packed.size() / format.bpb * 4 : packed.size()};
        assert(!std::memcmp(staging->data(), reference.data(), depthBytes));
        if (combined) {
            size_t offset{(packed.size() + 3) & ~size_t{3}};
            assert(!std::memcmp(staging->data() + offset, reference.data() + offset, packed.size() / format.bpb));
        }
    }
    if (only != "upload") {
        // Simulate new GPU image contents with an independently assembled aspect buffer.
        for (auto &byte : packed) byte ^= 0x3D;
        if (format.bpb == 8) for (size_t i{}; i < packed.size(); i += 8)
            packed[i + 5] = packed[i + 6] = packed[i + 7] = 0;
        auto readback{ReferenceTransfer(packed, format)};
        auto expectedGuest{texture.mirror};
        putGuest(packed);
        std::swap(expectedGuest, texture.mirror);
        texture.CopyToGuest(readback.data());
        assert(texture.mirror == expectedGuest); // Includes row and layer padding preservation.
    }
    std::printf("%s stencilFirst=%d layers=%zu mode=%s %s passed\n", vk::to_string(format.vkFormat).c_str(),
        format.stencilFirst, layers, mode == texture::TileMode::Linear ? "Linear" : "Pitch", only.empty() ? "upload/readback" : only.c_str());
}
}
int main(int argc, char **argv) {
    using namespace skyline::gpu;
    auto depth{vk::ImageAspectFlagBits::eDepth}, stencil{vk::ImageAspectFlagBits::eStencil};
    std::string only{argc > 1 ? argv[1] : ""};
    if (!only.empty()) {
        Check({vk::Format::eS8Uint, stencil, 1}, 3, texture::TileMode::Linear, only);
    } else {
        for (size_t layers : {1U, 3U})
            for (auto mode : {texture::TileMode::Linear, texture::TileMode::Pitch})
                for (Format format : {Format{vk::Format::eD24UnormS8Uint, depth | stencil, 4},
                    Format{vk::Format::eD24UnormS8Uint, depth | stencil, 4, true},
                    Format{vk::Format::eD32SfloatS8Uint, depth | stencil, 8},
                    Format{vk::Format::eD16Unorm, depth, 2}, Format{vk::Format::eS8Uint, stencil, 1}})
                    Check(format, layers, mode, only);
    }
    std::puts("PASS actual SynchronizeHostImpl/CopyToGuest linear and pitched guest data under ASan/UBSan");
}
