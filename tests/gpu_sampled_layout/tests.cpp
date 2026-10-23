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
                   size_t heightGobs, size_t depthGobs, size_t tileWidthSpacing = 0) {
        size_t widthGobs{(rowBytes + 63) / 64};
        // Independent guest memory address oracle from the TIC's GOB
        // alignment. BC1 1920x1080 at spacing=3: 60 -> 64 GOB columns.
        const size_t spacingGobs{size_t{1} << tileWidthSpacing};
        if (tileWidthSpacing && widthGobs > spacingGobs &&
            rows > heightGobs * 8 && depthGobs == 1)
            widthGobs = ((widthGobs + spacingGobs - 1) / spacingGobs) * spacingGobs;
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
                        const size_t tiled{layer * declaredStride + Address(x, y, z, rowBytes, rows, effectiveHeight, declaredDepth,
                                                   tic.tileConfig.sparseTileWidthGobsLog2)};
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
        std::vector<u8> written(source.size(), 0xEA);
        for (size_t layer{}; layer < guest.layerCount; ++layer)
            texture::CopyLinearToBlockLinear(guest, actual.data() + layer * packedSize,
                                            written.data() + layer * guest.GetLayerStride());
        Check(written == source, std::string(name) + ": readback changed pixels or padding");
        Check(guest.tileConfig.blockHeight == effectiveHeight, std::string(name) + ": effective Y");
        Check(guest.tileConfig.blockDepth == declaredDepth, std::string(name) + ": preserved Z");
        Check(guest.tileConfig.tileWidthSpacing == tic.tileConfig.sparseTileWidthGobsLog2,
              std::string(name) + ": horizontal TIC GOB spacing");
        Check(guest.GetLayerStride() == declaredStride, std::string(name) + ": declared layer stride");
        Check(guest.GetSize() == size_t{declaredStride} * guest.layerCount, std::string(name) + ": declared allocation");
        ++cases;
        std::cout << "PASS: " << name << " packed=" << expected.size() << " stride=" << declaredStride << '\n';
    }
}
int main() {
    try {
        const texture::Format bc3{4, 4, 16}, bc1{4, 4, 8}, rgba{1, 1, 4}, astc5x5{5, 5, 16};
        const texture::Format depth{1, 1, 4, vk::ImageAspectFlagBits::eDepth};
        // Regression: the 1920px Aniplex BC1 asset has 3840 logical
        // bytes/row but the TIC requires 4096 bytes in block-linear storage.
        // Ignoring the spacing introduced one corrupt 16-KiB rectangle per
        // 256-KiB span, moving 128 pixels right every 256 rows.
        auto aniplex{Header(1920, 1080, 1, 3)};
        aniplex.tileConfig.sparseTileWidthGobsLog2 = 3;
        UploadCase("Aniplex 1920x1080 BC1 spacing=3", aniplex, bc1, 8, 1310720);
        UploadCase("same BC1 without spacing unchanged", Header(1920, 1080, 1, 3), bc1, 8, 1228800);
        auto wideBc3{Header(513, 257, 1, 3)};
        wideBc3.tileConfig.sparseTileWidthGobsLog2 = 3;
        UploadCase("odd BC3 spacing upload/readback", wideBc3, bc3, 8, 327680);
        auto wideAstc{Header(1001, 401, 1, 2)};
        wideAstc.tileConfig.sparseTileWidthGobsLog2 = 3;
        UploadCase("odd ASTC spacing upload/readback", wideAstc, astc5x5, 4, 344064);
        auto volume{Header(960, 270, 4, 3, TextureImageControl::TextureType::e3D)};
        volume.tileConfig.sparseTileWidthGobsLog2 = 3;
        UploadCase("3D spaced planes upload/readback", volume, rgba, 8, 5242880);
        Check(texture::GetBlockLinearDepthSliceOffset({960,270,4}, 1,1,4, 8,1,1,3) == 1310720 &&
              texture::GetBlockLinearDepthSliceOffset({960,270,4}, 1,1,4, 8,1,1,0) == 1228800,
              "3D slice origin ignored padded physical width");
        auto narrow{Header(64, 32, 1, 3)};
        narrow.tileConfig.sparseTileWidthGobsLog2 = 3;
        UploadCase("small BC1 skips extra horizontal alignment", narrow, bc1, 1);
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
        // Literal offsets/strides independently calculated from Ryujinx's
        // SizeCalculator: floor pixel mips before compression-block rounding.
        struct MipCase {
            texture::Dimensions dimensions;
            const texture::Format *format;
            size_t height, depth, spacing;
            std::vector<size_t> sizes, effectiveSpacing;
            size_t layerStride;
        };
        for (const auto &test : std::vector<MipCase>{
                {{1920,1080,1}, &bc1, 8,1,3, {1310720,393216,131072,32768,8192,2048,512,512}, {3,3,3,0,0,0,0,0}, 1900544},
                {{513,257,1}, &bc3, 8,1,3, {327680,32768,8192,2048}, {3,0,0,0}, 393216},
                {{131,83,1}, &astc5x5, 4,1,3, {14336,4096,1024,512,512}, {0,0,0,0,0}, 32768},
                {{528,128,8}, &rgba, 2,4,3, {2621440,393216,36864,5120}, {3,3,0,0}, 3080192}}) {
            const auto &f{*test.format};
            const auto levels{texture::GetBlockLinearMipLayout(test.dimensions, f.blockHeight,f.blockWidth,f.bpb,
                f.blockHeight,f.blockWidth,f.bpb, test.height,test.depth,test.sizes.size(),test.spacing)};
            for (size_t i{}; i < levels.size(); ++i) {
                Check(levels[i].blockLinearSize == test.sizes[i], "physical mip size mismatch at level " + std::to_string(i));
                Check(levels[i].tileWidthSpacing == test.effectiveSpacing[i], "mip spacing eligibility mismatch");
            }
            Check(texture::GetBlockLinearLayerSize(test.dimensions, f.blockHeight,f.blockWidth,f.bpb,
                test.height,test.depth,test.sizes.size(),true,test.spacing) == test.layerStride,
                "array layer stride omits tile-width alignment");
        }
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
        std::cout << "PASS: " << cases << " oracle upload/readbacks; four mip/array layouts; spaced 3D offsets; pitch/RT/DMA controls\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
