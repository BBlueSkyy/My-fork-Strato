// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include "output_surface.h"

namespace skyline::soc::host1x::nvdec {
    OutputSurface GetH264OutputSurface(const Registers &registers, const H264ParameterSet &params) {
        size_t index{static_cast<size_t>(params.currPicIdx)};
        if (index >= registers.surfaceLumaOffsets.size() || !params.picWidthInMbs || !params.frameHeightInMbs ||
            params.picWidthInMbs > 1024 || params.frameHeightInMbs > 1024)
            throw std::invalid_argument("Invalid H264 output surface index or dimensions");

        u64 luma{registers.surfaceLumaOffsets[index].Address()};
        u64 chroma{registers.surfaceChromaOffsets[index].Address()};
        OutputSurface surface{
            .width = params.picWidthInMbs * 16,
            .height = params.frameHeightInMbs * 16,
            .lumaPitch = params.pitchLuma,
            .chromaPitch = params.pitchChroma,
            .nv24 = params.outputMemoryLayout != 0,
            .fieldSurfaces = !params.frameMbsOnlyFlag && !params.frameSurfaces &&
                             (params.lumaTopOffset.raw || params.lumaBotOffset.raw ||
                              params.chromaTopOffset.raw || params.chromaBotOffset.raw),
            .luma = {luma + params.lumaFrameOffset.Address(), 0},
            .chroma = {chroma + params.chromaFrameOffset.Address(), 0},
        };
        // An interlaced sequence (including MBAFF) can use a woven frame.
        // Zero field offsets do not describe two fields at the same address.
        if (surface.fieldSurfaces) {
            surface.luma = {luma + params.lumaTopOffset.Address(), luma + params.lumaBotOffset.Address()};
            surface.chroma = {chroma + params.chromaTopOffset.Address(), chroma + params.chromaBotOffset.Address()};
        }

        // T210 has a fixed two-GOB, 16Bx2-sector block-linear decode surface.
        // tileFormat/gobHeight are not a pitch selector: configurable tiling in
        // nvdec_drv.h is NVDEC3+ state, absent on this emulated hardware generation.
        return surface;
    }
}
