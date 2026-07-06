// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <soc.h>
#include <gpu/texture/layout.h>
#include "surface_writer.h"

namespace skyline::soc::host1x::vic {
    /**
     * @brief Writes a fully populated linear plane to the guest surface plane at the supplied IOVA, swizzling it into block-linear layout when required
     * @param linear A linear buffer holding the plane at a tight stride for block-linear surfaces or the aligned output stride for pitch surfaces
     */
    static void WritePlane(const DeviceState &state, u64 iova, span<u8> linear, u32 width, u32 height, u32 bpb, BlkKind blkKind, u32 blkHeightLog2) {
        if (blkKind == BlkKind::Pitch) {
            state.soc->smmu.Write(static_cast<u32>(iova), linear);
        } else {
            gpu::texture::Dimensions dimensions{width, height, 1};
            size_t gobBlockHeight{1UL << blkHeightLog2};

            std::vector<u8> swizzled(gpu::texture::GetBlockLinearLayerSize(dimensions, 1, 1, bpb, gobBlockHeight, 1));
            gpu::texture::CopyLinearToBlockLinear(dimensions, 1, 1, bpb, gobBlockHeight, 1, linear.data(), swizzled.data());

            state.soc->smmu.Write(static_cast<u32>(iova), span<u8>(swizzled));
        }
    }

    /**
     * @return The row stride for a plane given the surface layout, pitch surfaces are aligned to 16 bytes while block-linear planes are kept tight for swizzling
     */
    static u32 PlaneStride(u32 width, u32 bpb, BlkKind blkKind) {
        return blkKind == BlkKind::Pitch ? util::AlignUp(width * bpb, 0x10) : width * bpb;
    }

    void WriteNv12Surface(const DeviceState &state, const ConfigStruct &config, const PlaneOffsets &outputSurface, AVFrame *frame) {
        auto &surfaceConfig{config.outputSurfaceConfig};
        BlkKind blkKind{surfaceConfig.outBlkKind};
        u32 blkHeight{surfaceConfig.outBlkHeight};

        u32 lumaWidth{u32{surfaceConfig.outLumaWidth} + 1}, lumaHeight{u32{surfaceConfig.outLumaHeight} + 1};
        u32 chromaWidth{u32{surfaceConfig.outChromaWidth} + 1}, chromaHeight{u32{surfaceConfig.outChromaHeight} + 1};

        u32 lumaStride{PlaneStride(lumaWidth, 1, blkKind)};
        u32 chromaStride{PlaneStride(chromaWidth, 2, blkKind)};

        std::vector<u8> luma(static_cast<size_t>(lumaStride) * lumaHeight);
        std::vector<u8> chroma(static_cast<size_t>(chromaStride) * chromaHeight);

        bool planarInput{frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P)};
        bool semiPlanarInput{frame && frame->format == AV_PIX_FMT_NV12};

        if (planarInput || semiPlanarInput) {
            u32 copyWidth{std::min(static_cast<u32>(frame->width), lumaWidth)};
            u32 copyHeight{std::min(static_cast<u32>(frame->height), lumaHeight)};

            for (u32 y{}; y < copyHeight; y++)
                std::memcpy(luma.data() + static_cast<size_t>(y) * lumaStride, frame->data[0] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[0]), copyWidth);

            u32 chromaCopyWidth{std::min(static_cast<u32>(frame->width) / 2, chromaWidth)};
            u32 chromaCopyHeight{std::min(static_cast<u32>(frame->height) / 2, chromaHeight)};

            if (semiPlanarInput) {
                for (u32 y{}; y < chromaCopyHeight; y++)
                    std::memcpy(chroma.data() + static_cast<size_t>(y) * chromaStride, frame->data[1] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[1]), static_cast<size_t>(chromaCopyWidth) * 2);
            } else {
                for (u32 y{}; y < chromaCopyHeight; y++) {
                    u8 *dst{chroma.data() + static_cast<size_t>(y) * chromaStride};
                    const u8 *chromaU{frame->data[1] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[1])};
                    const u8 *chromaV{frame->data[2] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[2])};

                    for (u32 x{}; x < chromaCopyWidth; x++) {
                        dst[x * 2] = chromaU[x];
                        dst[x * 2 + 1] = chromaV[x];
                    }
                }
            }
        } else {
            if (frame)
                LOGW("Unsupported decoded frame format for NV12 write: {}", frame->format);

            // A solid magenta test pattern keeps the output chain observable when no frame is available
            constexpr u8 TestLuma{106}, TestChromaU{202}, TestChromaV{222};
            std::memset(luma.data(), TestLuma, luma.size());
            for (size_t i{}; i < chroma.size(); i += 2) {
                chroma[i] = TestChromaU;
                chroma[i + 1] = TestChromaV;
            }
        }

        WritePlane(state, outputSurface.luma.Address(), span<u8>(luma), lumaWidth, lumaHeight, 1, blkKind, blkHeight);
        WritePlane(state, outputSurface.chromaU.Address(), span<u8>(chroma), chromaWidth, chromaHeight, 2, blkKind, blkHeight);
    }

    void WriteRgbaSurface(const DeviceState &state, const ConfigStruct &config, const PlaneOffsets &outputSurface, AVFrame *frame) {
        auto &surfaceConfig{config.outputSurfaceConfig};
        BlkKind blkKind{surfaceConfig.outBlkKind};
        u32 blkHeight{surfaceConfig.outBlkHeight};

        u32 width{u32{surfaceConfig.outLumaWidth} + 1}, height{u32{surfaceConfig.outLumaHeight} + 1};
        u32 stride{PlaneStride(width, 4, blkKind)};

        std::vector<u8> surface(static_cast<size_t>(stride) * height);

        // A8B8G8R8 style formats hold bytes R,G,B,A in memory while A8R8G8B8 style formats hold B,G,R,A
        bool bgraOutput{surfaceConfig.outPixelFormat == VideoPixelFormat::A8R8G8B8 || surfaceConfig.outPixelFormat == VideoPixelFormat::X8R8G8B8};
        AVPixelFormat destinationFormat{bgraOutput ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA};

        SwsContext *converter{frame ? sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                                     static_cast<int>(width), static_cast<int>(height), destinationFormat,
                                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr) : nullptr};

        if (converter) {
            std::array<u8 *, 1> destinationData{surface.data()};
            std::array<int, 1> destinationStride{static_cast<int>(stride)};
            sws_scale(converter, frame->data, frame->linesize, 0, frame->height, destinationData.data(), destinationStride.data());
            sws_freeContext(converter);
        } else {
            if (frame)
                LOGW("Failed to create a converter for decoded frame format: {}", frame->format);

            // A solid magenta test pattern, the byte pattern is magenta under both RGBA and BGRA byte orders
            for (size_t i{}; i < surface.size(); i += 4) {
                surface[i] = 0xFF;
                surface[i + 1] = 0;
                surface[i + 2] = 0xFF;
                surface[i + 3] = 0xFF;
            }
        }

        WritePlane(state, outputSurface.luma.Address(), span<u8>(surface), width, height, 4, blkKind, blkHeight);
    }
}
