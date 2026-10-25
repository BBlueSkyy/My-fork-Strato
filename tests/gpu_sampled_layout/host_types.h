#pragma once
// SPDX-License-Identifier: MPL-2.0
// Only Vulkan/resource interfaces are facades: TIC bits, construction arithmetic,
// size/stride calculations and block-linear copies come from production sources.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <gpu/texture/pitch_linear.h>
namespace vk {
    enum class Format { eR8G8B8A8Unorm, eD16Unorm, eD32Sfloat, eX8D24UnormPack32, eD24UnormS8Uint, eS8Uint };
    enum class ImageType { e1D, e2D, e3D };
    enum class ImageAspectFlagBits { eColor, eDepth };
    enum class SampleCountFlagBits { e1 };
    enum class ImageTiling { eOptimal };
    struct ImageSubresourceRange {};
    enum class ImageViewType { e1D, e1DArray, e2D, e2DArray, e3D, eCube, eCubeArray };
}
namespace skyline {
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    struct exception : std::runtime_error {
        template<class... Args> exception(const char *message, Args...) : std::runtime_error(message) {}
    };
    namespace util {
        template<class T> constexpr T DivideCeil(T x, T y) { return x / y + (x % y != 0); }
        template<class T, class U> constexpr auto AlignUp(T x, U y) { return ((x + y - 1) / y) * y; }
        template<class T, class U> constexpr T BitCast(const U &value) { return std::bit_cast<T>(value); }
        template<class T> constexpr T AlignDown(T x, T y) { return x / y * y; }
        template<class T> constexpr bool IsAligned(T x, T y) { return x % y == 0; }
    }
    namespace gpu {
        namespace texture {
#include "tile.inc"
            struct Dimensions {
                u32 width, height, depth;
                explicit operator bool() const { return width && height && depth; }
                auto operator<=>(const Dimensions &) const = default;
            };
            struct Format {
                u32 blockWidth, blockHeight, bpb;
                vk::ImageAspectFlagBits vkAspect{vk::ImageAspectFlagBits::eColor};
                vk::Format vkFormat{vk::Format::eR8G8B8A8Unorm};
                bool IsCompatible(const Format &other) const { return this == &other; }
                size_t GetSize(Dimensions d) const {
                    return size_t{util::DivideCeil(d.width, blockWidth)} * util::DivideCeil(d.height, blockHeight) * bpb * d.depth;
                }
            };
        }
        struct Mapping : std::span<u8> {
            using std::span<u8>::span;
            bool valid() const { return data() != nullptr; }
        };
        struct GuestTexture {
            texture::Dimensions dimensions{};
            const texture::Format *format{};
            texture::TileConfig tileConfig{};
            vk::ImageViewType viewType{};
            u32 layerStride{}, layerCount{1}, baseArrayLayer{}, mipLevelCount{1}, viewMipBase{}, viewMipCount{1};
            std::vector<Mapping> mappings{};
            vk::ImageType GetImageType() const;
            u32 GetViewLayerCount() const;
            u32 GetViewDepth() const;
            u32 GetLayerStride();
            u32 CalculateLayerSize() const;
            size_t GetSize();
        };
    }
}
#include <gpu/interconnect/common/tic.h>
#include "layout.inc"
#include "guest.inc"
namespace skyline::gpu::interconnect {
    namespace Shader { enum class TextureType { Color2D, ColorArray1D, ColorArray2D, ColorArrayCube }; }
    GuestTexture Construct(const TextureImageControl &textureHeader, const texture::Format &format) {
        GuestTexture guest{.format = &format};
        const auto shaderType{Shader::TextureType::Color2D};
#include "tic-construction.inc"
        return guest;
    }
}
