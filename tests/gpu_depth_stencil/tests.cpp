// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>
#include <gpu/texture/depth_stencil.h>
using u32 = std::uint32_t;
namespace boost::container { template<class T, unsigned N> using small_vector = std::vector<T>; }
namespace skyline::gpu {
    namespace texture {
        struct FormatBase {
            unsigned bpb;
            vk::Format vkFormat;
            vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor};
            vk::ComponentMapping swizzleMapping{};
            bool stencilFirst{};
        };
    }
    namespace format {
        using vka = vk::ImageAspectFlagBits;
        constexpr vk::ImageAspectFlags Aspects(vk::Format format) {
            if (format == vk::Format::eS8Uint) return vka::eStencil;
            if (format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD32SfloatS8Uint)
                return vka::eDepth | vka::eStencil;
            return vka::eDepth;
        }
// Preserve production widths and VkFormat. View swizzles are irrelevant here.
#define FORMAT(name, bits, format, ...) constexpr texture::FormatBase name{bits / 8, vk::Format::format, Aspects(vk::Format::format)}
#include "formats.inc"
#undef FORMAT
    }
    struct Level { vk::Extent3D dimensions; std::size_t targetLinearSize; };
    struct Texture {
        const texture::FormatBase *format;
        u32 layerCount{2};
        std::vector<Level> mipLayouts;
        std::size_t surfaceSize{80};
        boost::container::small_vector<vk::BufferImageCopy, 10> GetBufferImageCopies();
    };
#include "regions.inc"
}
int main(int argc, char **argv) {
    using namespace skyline::gpu;
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
        const texture::FormatBase color{4, vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor};
        Texture ordinary{&color, 1, {{{256, 256, 1}, 262144}, {{128, 128, 1}, 65536}, {{64, 64, 1}, 16384}}};
        std::uint64_t checksum{};
        const auto iteration = [&] {
            const auto copies = ordinary.GetBufferImageCopies();
            checksum += copies.size() + copies.back().bufferOffset;
        };
        for (unsigned i{}; i < 100000; ++i) iteration();
        std::array<double, 7> samples;
        for (auto &sample : samples) {
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i{}; i < 5000000; ++i) iteration();
            sample = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 5000000;
        }
        std::cout << "color region generation ns/iteration:";
        for (auto sample : samples) std::cout << ' ' << sample;
        std::sort(samples.begin(), samples.end());
        std::cout << "; median=" << samples[3] << "; checksum=" << checksum << '\n';
        using namespace texture;
        const std::vector<Level> levels{{{1920, 1080, 1}, 1920 * 1080 * 4}};
        constexpr std::size_t packedSize{1920 * 1080 * 4};
        std::vector<std::uint8_t> data(GetDepthStencilTransferSize(packedSize, levels, 1, 4), 0x56);
        for (auto &sample : samples) {
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i{}; i < 10; ++i) {
                UnpackDepthStencil(data, DepthStencilPacking::Depth24Stencil8, levels, 1, packedSize);
                PackDepthStencil(data, DepthStencilPacking::Depth24Stencil8, levels, 1, packedSize);
                checksum += data[i];
            }
            sample = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 10;
        }
        std::cout << "1080p D24 unpack+pack ms/roundtrip:";
        for (auto sample : samples) std::cout << ' ' << sample;
        std::sort(samples.begin(), samples.end());
        std::cout << "; median=" << samples[3] << "; checksum=" << checksum << '\n';
        return 0;
    }
    Texture texture{&format::D24UnormS8Uint, 2, {{{3, 3, 1}, 36}, {{1, 1, 1}, 4}}};
    const auto regions = texture.GetBufferImageCopies();
    assert(regions.size() == 4);
    for (const auto &region : regions) assert(region.bufferOffset % 4 == 0);
    assert(regions[0].imageSubresource.aspectMask == vk::ImageAspectFlagBits::eDepth);
    assert(regions[2].imageSubresource.aspectMask == vk::ImageAspectFlagBits::eStencil);
    assert(regions[0].bufferOffset != regions[2].bufferOffset);
    assert(format::D32FloatS8Uint.bpb == 8);
    assert(format::S8Uint.bpb == 1);
    using namespace skyline::gpu::texture;
    for (const auto packing : {DepthStencilPacking::Depth24Stencil8, DepthStencilPacking::Stencil8Depth24,
                              DepthStencilPacking::Depth32FloatStencil8}) {
        const auto bytes = DepthStencilTexelBytes(packing);
        for (std::size_t layers : {1u, 2u, 3u}) {
            std::vector<Level> levels{{{3, 3, 1}, 9 * bytes}, {{1, 1, 1}, bytes}};
            const std::size_t packedSize = 10 * layers * bytes;
            const auto size = GetDepthStencilTransferSize(packedSize, levels, layers, bytes);
            std::vector<std::uint8_t> buffer(size + 16, 0xCD);
            auto data = std::span(buffer).first(size);
            for (std::size_t i{}; i < 10 * layers; ++i) {
                const std::uint32_t depth = packing == DepthStencilPacking::Depth32FloatStencil8 ?
                    0x3E800000u + std::uint32_t(i * 4096) : std::uint32_t(0x123456 + i * 17);
                const std::uint32_t stencil = (i * 31 + 7) & 255;
                const std::uint32_t word = packing == DepthStencilPacking::Stencil8Depth24 ? depth << 8 | stencil :
                    packing == DepthStencilPacking::Depth24Stencil8 ? depth | stencil << 24 : depth;
                std::memcpy(data.data() + i * bytes, &word, 4);
                if (bytes == 8) std::memcpy(data.data() + i * bytes + 4, &stencil, 4);
            }
            const auto expected = buffer;
            UnpackDepthStencil(data, packing, levels, layers, packedSize);
            ForEachDepthStencilCopyLevel(levels, layers, packedSize, bytes,
                [&](std::size_t mip, std::size_t depthOffset, std::size_t stencilOffset) {
                    assert(depthOffset % 4 == 0 && stencilOffset % 4 == 0);
                    const auto texels = levels[mip].targetLinearSize / bytes * layers;
                    assert(depthOffset + texels * 4 <= stencilOffset);
                    assert(stencilOffset + texels <= size);
                    std::uint32_t word;
                    std::memcpy(&word, data.data() + depthOffset, 4);
                    const auto first = mip ? 9 * layers : 0;
                    assert(word == (bytes == 8 ? 0x3E800000u + first * 4096 : 0x123456 + first * 17));
                    assert(data[stencilOffset] == ((first * 31 + 7) & 255));
                });
            PackDepthStencil(data, packing, levels, layers, packedSize);
            assert(std::equal(data.begin(), data.begin() + packedSize, expected.begin()));
            assert(std::all_of(buffer.begin() + size, buffer.end(), [](auto b) { return b == 0xCD; }));
            bool rejected{};
            try { UnpackDepthStencil(data.first(size - 1), packing, levels, layers, packedSize); }
            catch (const std::out_of_range &) { rejected = true; }
            assert(rejected);
        }
    }
    for (std::size_t bytes : {1u, 2u, 4u}) {
        std::vector<Level> levels{{{3, 3, 1}, 9 * bytes}, {{1, 1, 1}, bytes}, {{1, 1, 1}, bytes}};
        const auto size = GetSingleAspectTransferSize(levels, 3);
        std::vector<std::uint8_t> buffer(size);
        for (std::size_t i{}; i < 33 * bytes; ++i) buffer[i] = std::uint8_t(i * 19);
        const auto expected = buffer;
        UnpackSingleAspect(buffer, levels, 3);
        PackSingleAspect(buffer, levels, 3);
        assert(std::equal(buffer.begin(), buffer.begin() + 33 * bytes, expected.begin()));
    }
    std::cout << "depth/stencil disjoint regions, alignment and guest texel widths: PASS\n";
    return 0;
}
