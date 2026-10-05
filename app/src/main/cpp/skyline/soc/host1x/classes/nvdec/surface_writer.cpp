// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <soc/host1x/surface_writer.h>
#include "surface_writer.h"

namespace skyline::soc::host1x::nvdec {
    void WriteDecodedSurface(const DeviceState &state, const OutputSurface &surface, const AVFrame *frame) {
        if (!frame || frame->width <= 0 || frame->height <= 0 ||
            static_cast<u32>(frame->width) != surface.width || static_cast<u32>(frame->height) != surface.height)
            throw std::invalid_argument("NVDEC frame does not match its output surface dimensions");

        u32 chromaWidth{surface.nv24 ? surface.width : (surface.width + 1) / 2};
        u32 chromaHeight{surface.nv24 ? surface.height : (surface.height + 1) / 2};
        u32 fields{surface.fieldSurfaces ? 2U : 1U};
        if (surface.fieldSurfaces && ((surface.height | chromaHeight) & 1))
            throw std::invalid_argument("Invalid NVDEC field surface height");

        std::array<SurfacePlane, 4> planes{};
        std::array<std::vector<u8>, 4> buffers;
        for (u32 field{}; field < fields; field++) {
            planes[field * 2] = {surface.luma[field], surface.width, surface.height / fields, surface.lumaPitch, true, 1};
            planes[field * 2 + 1] = {surface.chroma[field], chromaWidth * 2, chromaHeight / fields, surface.chromaPitch, true, 1};
            ValidateSurfacePlane(state, planes[field * 2]);
            ValidateSurfacePlane(state, planes[field * 2 + 1]);
        }

        // Validate the complete allocation ranges, including block padding.
        // No field or chroma plane may overwrite another decoded plane.
        for (u32 i{}; i < fields * 2; i++) {
            u64 end{planes[i].address + GetSurfacePlaneSize(planes[i])};
            for (u32 j{}; j < i; j++)
                if (planes[i].address < planes[j].address + GetSurfacePlaneSize(planes[j]) && planes[j].address < end)
                    throw std::invalid_argument("Overlapping NVDEC output planes");
        }

        AVPixelFormat planarFormat{surface.nv24 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P};
        bool planar{frame->format == planarFormat ||
                    frame->format == (surface.nv24 ? AV_PIX_FMT_YUVJ444P : AV_PIX_FMT_YUVJ420P)};
        bool semiPlanar{frame->format == (surface.nv24 ? AV_PIX_FMT_NV24 : AV_PIX_FMT_NV12)};
        std::unique_ptr<AVFrame, void (*)(AVFrame *)> converted{nullptr, [](AVFrame *ptr) { av_frame_free(&ptr); }};
        if (!planar && !semiPlanar) {
            converted.reset(av_frame_alloc());
            if (!converted)
                throw std::bad_alloc();
            converted->format = planarFormat;
            converted->width = frame->width;
            converted->height = frame->height;
            if (av_frame_get_buffer(converted.get(), 32) < 0)
                throw std::bad_alloc();
            std::unique_ptr<SwsContext, decltype(&sws_freeContext)> converter{
                sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                               frame->width, frame->height, planarFormat, SWS_POINT, nullptr, nullptr, nullptr),
                sws_freeContext};
            // Changing chroma sampling/layout must not remap decoded YUV
            // sample ranges. In particular YUVJ input must remain full-range.
            int fullRange{frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P ||
                          frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P};
            const int *coefficients{sws_getCoefficients(SWS_CS_DEFAULT)};
            if (!converter || sws_setColorspaceDetails(converter.get(), coefficients, fullRange, coefficients,
                                                       fullRange, 0, 1 << 16, 1 << 16) < 0 ||
                sws_scale(converter.get(), frame->data, frame->linesize, 0, frame->height,
                          converted->data, converted->linesize) != frame->height)
                throw std::invalid_argument("Unsupported NVDEC decoded pixel format");
            frame = converted.get();
        }

        if (!frame->data[0] || !frame->data[1] || (!semiPlanar && !frame->data[2]) ||
            std::abs(static_cast<i64>(frame->linesize[0])) < static_cast<int>(surface.width) ||
            std::abs(static_cast<i64>(frame->linesize[1])) < static_cast<int>(chromaWidth * (semiPlanar ? 2 : 1)) ||
            (!semiPlanar && std::abs(static_cast<i64>(frame->linesize[2])) < static_cast<int>(chromaWidth)))
            throw std::invalid_argument("Invalid NVDEC decoded plane data");

        for (u32 field{}; field < fields; field++) {
            auto &luma{buffers[field * 2]};
            auto &chroma{buffers[field * 2 + 1]};
            luma.resize(static_cast<size_t>(surface.lumaPitch) * (surface.height / fields));
            chroma.resize(static_cast<size_t>(surface.chromaPitch) * (chromaHeight / fields));
            for (u32 y{}; y < surface.height / fields; y++)
                std::memcpy(luma.data() + static_cast<size_t>(y) * surface.lumaPitch,
                            frame->data[0] + static_cast<ptrdiff_t>(y * fields + field) * frame->linesize[0], surface.width);
            for (u32 y{}; y < chromaHeight / fields; y++) {
                u8 *dst{chroma.data() + static_cast<size_t>(y) * surface.chromaPitch};
                const u8 *u{frame->data[1] + static_cast<ptrdiff_t>(y * fields + field) * frame->linesize[1]};
                if (semiPlanar) {
                    std::memcpy(dst, u, chromaWidth * 2);
                } else {
                    const u8 *v{frame->data[2] + static_cast<ptrdiff_t>(y * fields + field) * frame->linesize[2]};
                    for (u32 x{}; x < chromaWidth; x++) {
                        dst[x * 2] = u[x];
                        dst[x * 2 + 1] = v[x];
                    }
                }
            }
        }
        for (u32 plane{}; plane < fields * 2; plane++)
            WriteSurfacePlane(state, planes[plane], span<u8>(buffers[plane]));
    }
}
