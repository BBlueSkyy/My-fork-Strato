// SPDX-License-Identifier: MPL-2.0
#include "host_types.h"
namespace {
    using namespace skyline;
    using namespace skyline::gpu;
    using namespace skyline::gpu::interconnect;
    void Check(bool condition, const std::string &message) {
        if (!condition) throw std::runtime_error(message);
    }
    // Independent Maxwell NVIDIA address oracle, expressed in byte coordinates.
    // GOB intra-address bit order: x[5], y[2:1], x[4], y[0], x[3:0].
    // Whole GOBs are ordered by Y within an X/Z block, X, block row, then Z block.
    size_t Address(size_t x, size_t y, size_t z, size_t rowBytes, size_t rows,
                   size_t heightGobs, size_t depthGobs) {
        const size_t widthGobs{(rowBytes + 63) / 64};
        const size_t blockRows{(rows + 8 * heightGobs - 1) / (8 * heightGobs)};
        const size_t blockBytes{512 * heightGobs * depthGobs};
        const size_t blockIndex{(z / depthGobs) * blockRows * widthGobs +
                                (y / (8 * heightGobs)) * widthGobs + x / 64};
        const size_t gobIndex{(z % depthGobs) * heightGobs + (y / 8) % heightGobs};
        const size_t within{((x & 32) << 3) + ((y & 6) << 5) + ((x & 16) << 1) +
                            ((y & 1) << 4) + (x & 15)};
        return blockIndex * blockBytes + gobIndex * 512 + within;
    }
    TextureImageControl Header(u32 width, u32 height, u32 depth, u8 heightLog2,
                               TextureImageControl::TextureType type = TextureImageControl::TextureType::e2D,
                               u8 depthLog2 = 0, u8 mipLevels = 1) {
        TextureImageControl tic{};
        tic.widthMinusOne = width - 1;
        tic.heightMinusOne = height - 1;
        tic.depthMinusOne = depth - 1;
        tic.textureType = type;
        tic.headerType = TextureImageControl::HeaderType::BlockLinear;
        tic.tileConfig.tileHeightGobsLog2 = heightLog2;
        tic.tileConfig.tileDepthGobsLog2 = depthLog2;
        tic.mipMaxLevels = mipLevels - 1;
        tic.viewConfig.mipMaxLevel = mipLevels - 1;
        return tic;
    }
    // Independent byte-address oracle over every mip/face of the observed
    // cube shape. Test native FP16 and compressed block geometry separately;
    // this does not assert that the game's guest texture is BC6H.
    void CubemapMipCase(const texture::Format &format, size_t declaredHeight) {
        const texture::Dimensions dimensions{128, 128, 1};
        const auto levels{texture::GetBlockLinearMipLayout(dimensions,
            format.blockHeight, format.blockWidth, format.bpb,
            format.blockHeight, format.blockWidth, format.bpb, declaredHeight, 1, 8)};
        Check(levels.size() == 8, "cube mip count");
        size_t expectedHeight{declaredHeight};
        for (size_t mip{}; mip < 8; ++mip) {
            const size_t extent{128U >> mip};
            const size_t rows{(extent + format.blockHeight - 1) / format.blockHeight};
            const size_t rowBytes{((extent + format.blockWidth - 1) / format.blockWidth) * format.bpb};
            const size_t tiledSize{((rowBytes + 63) / 64) * 64 *
                ((rows + expectedHeight * 8 - 1) / (expectedHeight * 8)) * expectedHeight * 8};
            const auto &level{levels[mip]};
            Check(level.dimensions.width == extent && level.dimensions.height == extent &&
                  level.blockHeight == expectedHeight && level.blockLinearSize == tiledSize &&
                  level.linearSize == rowBytes * rows, "cube mip layout vs independent sizing");
            for (size_t face{}; face < 6; ++face) {
                std::vector<u8> raw(tiledSize, 0xEA), expected(rowBytes * rows), actual(expected.size(), 0xCC);
                for (size_t y{}; y < rows; ++y)
                    for (size_t x{}; x < rowBytes; ++x) {
                        const auto byte{static_cast<u8>((x * 37 + y * 11 + mip * 19 + face * 53) % 229)};
                        expected[y * rowBytes + x] = byte;
                        raw.at(Address(x, y, 0, rowBytes, rows, expectedHeight, 1)) = byte;
                    }
                texture::CopyBlockLinearToLinear(level.dimensions, format.blockWidth, format.blockHeight,
                    format.bpb, level.blockHeight, level.blockDepth, raw.data(), actual.data());
                Check(actual == expected, "cube mip/face deswizzle differs from address oracle");
            }
            const size_t nextRows{((std::max(extent / 2, size_t{1})) + format.blockHeight - 1) / format.blockHeight};
            while (expectedHeight > 1 && nextRows <= (expectedHeight / 2) * 8) expectedHeight /= 2;
        }
        std::cout << "PASS: 128x128 cube: all 8 mips x 6 faces; bytes/block=" << format.bpb << '\n';
    }
    size_t cases{};
    void UploadCase(const char *name, TextureImageControl tic, const texture::Format &format,
                    u8 effectiveHeight, u32 expectedStride = 0) {
        auto guest{Construct(tic, format)};
        const size_t declaredHeight{1UL << tic.tileConfig.tileHeightGobsLog2};
        const size_t declaredDepth{1UL << tic.tileConfig.tileDepthGobsLog2};
        auto raw{guest};
        raw.tileConfig.blockHeight = declaredHeight;
        raw.layerStride = 0;
        const auto declaredStride{raw.GetLayerStride()};
        if (expectedStride) Check(declaredStride == expectedStride, std::string(name) + ": fixture allocation");
        // Check actual copy first: RED must demonstrate wrong bytes, not just metadata.
        const size_t rows{(guest.dimensions.height + format.blockHeight - 1) / format.blockHeight};
        const size_t rowBytes{((guest.dimensions.width + format.blockWidth - 1) / format.blockWidth) * format.bpb};
        const size_t packedSize{rowBytes * rows * guest.dimensions.depth};
        std::vector<u8> source(size_t{declaredStride} * guest.layerCount, 0xEA);
        std::vector<u8> expected(packedSize * guest.layerCount), actual(expected.size(), 0xCC);
        for (size_t layer{}; layer < guest.layerCount; ++layer)
            for (size_t z{}; z < guest.dimensions.depth; ++z)
                for (size_t y{}; y < rows; ++y)
                    for (size_t x{}; x < rowBytes; ++x) {
                        const size_t linear{layer * packedSize + (z * rows + y) * rowBytes + x};
                        const size_t tiled{layer * declaredStride + Address(x, y, z, rowBytes, rows, effectiveHeight, declaredDepth)};
                        Check(tiled < source.size(), std::string(name) + ": oracle allocation");
                        expected[linear] = static_cast<u8>((x * 37 + y * 11 + z * 29 + layer * 53) % 229);
                        source[tiled] = expected[linear];
                    }
        auto *pointer{source.data()};
        for (size_t layer{}; layer < guest.layerCount; ++layer) {
            texture::CopyBlockLinearToLinear(guest, pointer, actual.data() + layer * packedSize);
            pointer += guest.GetLayerStride();
        }
        const auto mismatch{std::mismatch(actual.begin(), actual.end(), expected.begin())};
        Check(mismatch.first == actual.end(), std::string(name) + ": packed upload differs at byte " +
              std::to_string(mismatch.first - actual.begin()));
        Check(guest.tileConfig.blockHeight == effectiveHeight, std::string(name) + ": effective Y");
        Check(guest.tileConfig.blockDepth == declaredDepth, std::string(name) + ": preserved Z");
        Check(guest.GetLayerStride() == declaredStride, std::string(name) + ": declared layer stride");
        Check(guest.GetSize() == size_t{declaredStride} * guest.layerCount, std::string(name) + ": declared allocation");
        ++cases;
        std::cout << "PASS: " << name << " packed=" << expected.size() << " stride=" << declaredStride << '\n';
    }
}
int main() {
    try {
        const texture::Format bc3{4, 4, 16}, rgba{1, 1, 4}, astc5x5{5, 5, 16};
        CubemapMipCase(texture::Format{1, 1, 8}, 16);
        CubemapMipCase(texture::Format{4, 4, 16}, 4);
        const texture::Format depth{1, 1, 4, vk::ImageAspectFlagBits::eDepth};
        UploadCase("512x128 BC3 BH16 -> BH4", Header(512, 128, 1, 4), bc3, 4, 262144);
        UploadCase("odd BC3 height rounds compressed rows up", Header(513, 129, 1, 4), bc3, 8);
        UploadCase("tiny BC3 BH16 -> BH1", Header(7, 5, 1, 4), bc3, 1);
        UploadCase("odd ASTC 5x5 compression", Header(63, 41, 1, 4), astc5x5, 2);
        UploadCase("RGBA Y clamp", Header(257, 17, 1, 4), rgba, 4);
        UploadCase("BC3 array declared layer stride", Header(512, 128, 3, 4, TextureImageControl::TextureType::e2DArray), bc3, 4, 262144);
        UploadCase("cube declared face stride", Header(128, 128, 1, 4, TextureImageControl::TextureType::eCube), bc3, 4);
        UploadCase("3D Y and Z layouts remain declared", Header(128, 65, 3, 4, TextureImageControl::TextureType::e3D, 2), bc3, 16);
        UploadCase("1D Y layout remains declared", Header(512, 1, 1, 4, TextureImageControl::TextureType::e1D), rgba, 16);
        UploadCase("2D interleaved Z layout remains declared", Header(128, 32, 1, 4, TextureImageControl::TextureType::e2D, 2), rgba, 16);
        UploadCase("depth aspect layout remains declared", Header(512, 32, 1, 4), depth, 16);
        UploadCase("already small BH remains small", Header(512, 128, 1, 1), bc3, 2);
        UploadCase("height requires declared BH", Header(128, 512, 1, 4), bc3, 16);
        UploadCase("e2DNoMipmap overrides header mip count", Header(512, 128, 1, 4, TextureImageControl::TextureType::e2DNoMipmap, 0, 4), bc3, 4);
        auto arrayBase{Header(512, 128, 3, 4, TextureImageControl::TextureType::e2DArray)};
        arrayBase.viewLayerBase_0_2 = 1;
        auto based{Construct(arrayBase, bc3)};
        Check(based.baseArrayLayer == 1 && based.GetLayerStride() == 262144 &&
              based.GetSize() == 524288 && based.tileConfig.blockHeight == 16,
              "base array layer changed declared mapping extent");
        auto multi{Construct(Header(512, 128, 1, 4, TextureImageControl::TextureType::e2D, 0, 3), bc3)};
        Check(multi.tileConfig.blockHeight == 16 && multi.mipLevelCount == 3 && multi.layerStride == 0,
              "mipmapped TIC layout changed");
        auto pitch{Header(128, 16, 1, 0)};
        pitch.headerType = TextureImageControl::HeaderType::Pitch;
        pitch.tileConfig.pitchHigh = 32;
        auto pitched{Construct(pitch, rgba)};
        Check(pitched.tileConfig.mode == texture::TileMode::Pitch && pitched.layerStride == 0 &&
              pitched.tileConfig.pitch == 1024 && pitched.GetSize() == 16384, "pitch TIC changed");
        // Explicit BH for render-target/DMA callers must remain authoritative.
        auto explicitLayout{Construct(Header(512, 128, 1, 4), bc3)};
        explicitLayout.tileConfig.blockHeight = 16;
        explicitLayout.layerStride = 0;
        Check(explicitLayout.GetSize() == 262144, "low-level explicit BH allocation changed");
        const auto offset{Address(64, 0, 0, 2048, 32, 16, 1)};
        std::vector<u8> raw(explicitLayout.GetSize(), 0xEA), linear(65536);
        raw[offset] = 0x7B;
        texture::CopyBlockLinearToLinear(explicitLayout, raw.data(), linear.data());
        Check(linear[64] == 0x7B, "low-level explicit BH addressing changed");
        std::cout << "PASS: " << cases << " oracle uploads; mipmapped/pitch/explicit render-target-DMA layout controls\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
