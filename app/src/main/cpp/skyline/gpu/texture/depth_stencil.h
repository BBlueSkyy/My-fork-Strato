// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace skyline::gpu::texture {
    // Order in a little-endian guest texel, not Vulkan's format component names.
    enum class DepthStencilPacking { Depth24Stencil8, Stencil8Depth24, Depth32FloatStencil8 };

    inline std::size_t DepthStencilCheckedAdd(std::size_t a, std::size_t b) {
        if (b > std::numeric_limits<std::size_t>::max() - a)
            throw std::overflow_error("Depth/stencil transfer size overflow");
        return a + b;
    }

    inline std::size_t DepthStencilAlign(std::size_t size) {
        return DepthStencilCheckedAdd(size, 3) & ~std::size_t{3};
    }

    inline std::size_t DepthStencilTexelBytes(DepthStencilPacking packing) {
        return packing == DepthStencilPacking::Depth32FloatStencil8 ? 8 : 4;
    }

    template<class Level>
    std::size_t DepthStencilLevelTexels(const Level &level, std::size_t layers, std::size_t packedBytes) {
        if (!layers || (packedBytes != 4 && packedBytes != 8) || level.targetLinearSize % packedBytes)
            throw std::invalid_argument("Invalid depth/stencil transfer layout");
        const auto texels{level.targetLinearSize / packedBytes};
        if (texels > std::numeric_limits<std::size_t>::max() / layers)
            throw std::overflow_error("Depth/stencil layer size overflow");
        return texels * layers;
    }

    // Stencil is placed after the whole packed guest representation. This permits
    // in-place depth compaction on upload and reverse expansion on readback.
    // Each mip's stencil plane starts at a 4-byte boundary, including tiny mips.
    template<class Levels>
    std::size_t GetDepthStencilTransferSize(std::size_t packedSize, const Levels &levels,
                                          std::size_t layers, std::size_t packedBytes) {
        if (!layers || (packedBytes != 4 && packedBytes != 8))
            throw std::invalid_argument("Invalid depth/stencil texel size");
        std::size_t size{DepthStencilAlign(packedSize)}, texelCount{};
        for (const auto &level : levels) {
            const auto texels{DepthStencilLevelTexels(level, layers, packedBytes)};
            texelCount = DepthStencilCheckedAdd(texelCount, texels);
            size = DepthStencilCheckedAdd(size, DepthStencilAlign(texels));
        }
        if (texelCount != packedSize / packedBytes || packedSize % packedBytes)
            throw std::invalid_argument("Depth/stencil packed size does not match its levels");
        return size;
    }

    template<class Level>
    std::size_t SingleAspectLevelBytes(const Level &level, std::size_t layers) {
        if (!layers || level.targetLinearSize > std::numeric_limits<std::size_t>::max() / layers)
            throw std::overflow_error("Depth/stencil layer size overflow");
        return level.targetLinearSize * layers;
    }

    template<class Levels>
    std::size_t GetSingleAspectTransferSize(const Levels &levels, std::size_t layers) {
        std::size_t size{};
        for (const auto &level : levels)
            size = DepthStencilCheckedAdd(size, DepthStencilAlign(SingleAspectLevelBytes(level, layers)));
        return size;
    }

    template<class Levels>
    void UnpackSingleAspect(std::span<std::uint8_t> buffer, const Levels &levels, std::size_t layers) {
        auto transferEnd{GetSingleAspectTransferSize(levels, layers)};
        if (buffer.size() < transferEnd)
            throw std::out_of_range("Depth/stencil upload buffer is too small");
        std::size_t packedEnd{};
        for (const auto &level : levels) packedEnd += SingleAspectLevelBytes(level, layers);
        for (std::size_t mip{levels.size()}; mip--;) {
            const auto size{SingleAspectLevelBytes(levels[mip], layers)};
            packedEnd -= size;
            transferEnd -= DepthStencilAlign(size);
            if (packedEnd != transferEnd)
                std::memmove(buffer.data() + transferEnd, buffer.data() + packedEnd, size);
        }
    }

    template<class Levels>
    void PackSingleAspect(std::span<std::uint8_t> buffer, const Levels &levels, std::size_t layers) {
        if (buffer.size() < GetSingleAspectTransferSize(levels, layers))
            throw std::out_of_range("Depth/stencil readback buffer is too small");
        std::size_t packedOffset{}, transferOffset{};
        for (const auto &level : levels) {
            const auto size{SingleAspectLevelBytes(level, layers)};
            if (packedOffset != transferOffset)
                std::memmove(buffer.data() + packedOffset, buffer.data() + transferOffset, size);
            packedOffset += size;
            transferOffset += DepthStencilAlign(size);
        }
    }

    template<class Levels, class Callback>
    void ForEachDepthStencilCopyLevel(const Levels &levels, std::size_t layers,
                                     std::size_t packedSize, std::size_t packedBytes, Callback callback) {
        (void)GetDepthStencilTransferSize(packedSize, levels, layers, packedBytes);
        std::size_t depthOffset{}, stencilOffset{DepthStencilAlign(packedSize)}, mip{};
        for (const auto &level : levels) {
            callback(mip++, depthOffset, stencilOffset);
            const auto texels{DepthStencilLevelTexels(level, layers, packedBytes)};
            depthOffset += texels * 4;
            stencilOffset += DepthStencilAlign(texels);
        }
    }

    template<class Levels>
    void UnpackDepthStencil(std::span<std::uint8_t> buffer, DepthStencilPacking packing,
                            const Levels &levels, std::size_t layers, std::size_t packedSize) {
        const auto bytes{DepthStencilTexelBytes(packing)};
        if (buffer.size() < GetDepthStencilTransferSize(packedSize, levels, layers, bytes))
            throw std::out_of_range("Depth/stencil upload buffer is too small");
        std::size_t packedOffset{};
        ForEachDepthStencilCopyLevel(levels, layers, packedSize, bytes,
            [&](std::size_t mip, std::size_t depthOffset, std::size_t stencilOffset) {
                const auto texels{DepthStencilLevelTexels(levels[mip], layers, bytes)};
                for (std::size_t i{}; i < texels; ++i) {
                    std::uint32_t depth;
                    std::memcpy(&depth, buffer.data() + packedOffset + i * bytes, 4);
                    std::uint8_t stencil;
                    if (packing == DepthStencilPacking::Depth32FloatStencil8) {
                        stencil = buffer[packedOffset + i * bytes + 4];
                    } else if (packing == DepthStencilPacking::Stencil8Depth24) {
                        stencil = static_cast<std::uint8_t>(depth);
                        depth >>= 8;
                    } else {
                        stencil = static_cast<std::uint8_t>(depth >> 24);
                        depth &= 0xFFFFFF;
                    }
                    buffer[stencilOffset + i] = stencil;
                    std::memcpy(buffer.data() + depthOffset + i * 4, &depth, 4);
                }
                packedOffset += texels * bytes;
            });
    }

    template<class Levels>
    void PackDepthStencil(std::span<std::uint8_t> buffer, DepthStencilPacking packing,
                          const Levels &levels, std::size_t layers, std::size_t packedSize) {
        const auto bytes{DepthStencilTexelBytes(packing)};
        const auto transferSize{GetDepthStencilTransferSize(packedSize, levels, layers, bytes)};
        if (buffer.size() < transferSize)
            throw std::out_of_range("Depth/stencil readback buffer is too small");
        std::size_t packedEnd{packedSize}, depthEnd{packedSize / bytes * 4}, stencilEnd{transferSize};
        for (std::size_t mip{levels.size()}; mip--;) {
            const auto texels{DepthStencilLevelTexels(levels[mip], layers, bytes)};
            packedEnd -= texels * bytes;
            depthEnd -= texels * 4;
            stencilEnd -= DepthStencilAlign(texels);
            for (std::size_t i{texels}; i--;) {
                std::uint32_t depth;
                std::memcpy(&depth, buffer.data() + depthEnd + i * 4, 4);
                const auto stencil{buffer[stencilEnd + i]};
                if (packing == DepthStencilPacking::Depth32FloatStencil8) {
                    // The upper 24 bits of the second word are unused, not stencil.
                    const std::uint32_t stencilWord{stencil};
                    std::memcpy(buffer.data() + packedEnd + i * bytes + 4, &stencilWord, 4);
                } else if (packing == DepthStencilPacking::Stencil8Depth24) {
                    depth = (depth & 0xFFFFFF) << 8 | stencil;
                } else {
                    depth = (depth & 0xFFFFFF) | std::uint32_t{stencil} << 24;
                }
                std::memcpy(buffer.data() + packedEnd + i * bytes, &depth, 4);
            }
        }
    }
}
