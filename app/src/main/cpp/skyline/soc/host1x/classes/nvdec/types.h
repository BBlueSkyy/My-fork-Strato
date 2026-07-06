// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief A 256-byte aligned IOVA as held within decoder picture information structures
     */
    struct PictureOffset {
        u32 raw;

        constexpr u64 Address() const {
            return static_cast<u64>(raw) << 8;
        }
    };
    static_assert(sizeof(PictureOffset) == sizeof(u32));

    #pragma pack(push, 1)

    /**
     * @brief The H.264 sequence/picture parameter fields the guest driver supplies for a decode operation
     * @url https://developer.nvidia.com/embedded/downloads (L4T multimedia nvdec_drv.h, nvdec_h264_pic_s)
     */
    struct H264ParameterSet {
        i32 log2MaxPicOrderCntLsbMinus4; // 0x00
        i32 deltaPicOrderAlwaysZeroFlag; // 0x04
        i32 frameMbsOnlyFlag; // 0x08
        u32 picWidthInMbs; // 0x0C
        u32 frameHeightInMbs; // 0x10
        union {
            u32 rawSurfaceFormat; // 0x14
            struct {
                u32 tileFormat : 2;
                u32 gobHeight : 3;
                u32 _pad0_ : 27;
            };
        };
        u32 entropyCodingModeFlag; // 0x18
        i32 picOrderPresentFlag; // 0x1C
        i32 numRefIdxL0DefaultActive; // 0x20
        i32 numRefIdxL1DefaultActive; // 0x24
        i32 deblockingFilterControlPresentFlag; // 0x28
        i32 redundantPicCntPresentFlag; // 0x2C
        u32 transform8x8ModeFlag; // 0x30
        u32 pitchLuma; // 0x34
        u32 pitchChroma; // 0x38
        PictureOffset lumaTopOffset; // 0x3C
        PictureOffset lumaBotOffset; // 0x40
        PictureOffset lumaFrameOffset; // 0x44
        PictureOffset chromaTopOffset; // 0x48
        PictureOffset chromaBotOffset; // 0x4C
        PictureOffset chromaFrameOffset; // 0x50
        u32 histBufferSize; // 0x54
        union {
            u64 rawFlags; // 0x58
            struct {
                u64 mbaffFrame : 1;
                u64 direct8x8Inference : 1;
                u64 weightedPred : 1;
                u64 constrainedIntraPred : 1;
                u64 refPic : 1;
                u64 fieldPic : 1;
                u64 bottomField : 1;
                u64 secondField : 1;
                u64 log2MaxFrameNumMinus4 : 4;
                u64 chromaFormatIdc : 2;
                u64 picOrderCntType : 2;
                i64 picInitQpMinus26 : 6;
                i64 chromaQpIndexOffset : 5;
                i64 secondChromaQpIndexOffset : 5;
                u64 weightedBipredIdc : 2;
                u64 currPicIdx : 7;
                u64 currColIdx : 5;
                u64 frameNumber : 16;
                u64 frameSurfaces : 1;
                u64 outputMemoryLayout : 1;
            };
        };
    };
    static_assert(sizeof(H264ParameterSet) == 0x60);
    static_assert(offsetof(H264ParameterSet, pitchLuma) == 0x34);
    static_assert(offsetof(H264ParameterSet, rawFlags) == 0x58);

    /**
     * @brief An entry within the H.264 decoded picture buffer
     */
    struct DpbEntry {
        union {
            u32 rawFlags;
            struct {
                u32 index : 7;
                u32 colIdx : 5;
                u32 state : 2;
                u32 isLongTerm : 1;
                u32 nonExisting : 1;
                u32 isField : 1;
                u32 topFieldMarking : 4;
                u32 bottomFieldMarking : 4;
                u32 outputMemoryLayout : 1;
                u32 _pad0_ : 6;
            };
        };
        std::array<u32, 2> fieldOrderCnt;
        u32 frameIdx;
    };
    static_assert(sizeof(DpbEntry) == 0x10);

    /**
     * @brief The decoder context structure the guest driver writes for every H.264 decode operation
     */
    struct H264DecoderContext {
        std::array<u32, 13> _pad0_; // 0x00
        std::array<u8, 16> eos; // 0x34
        u8 explicitEosPresentFlag; // 0x44
        u8 hintDumpEnable; // 0x45
        std::array<u8, 2> _pad1_; // 0x46
        u32 streamLength; // 0x48
        u32 sliceCount; // 0x4C
        u32 mbhistBufferSize; // 0x50
        u32 gptimerTimeoutValue; // 0x54
        H264ParameterSet parameterSet; // 0x58
        std::array<i32, 2> currFieldOrderCnt; // 0xB8
        std::array<DpbEntry, 16> dpb; // 0xC0
        std::array<u8, 0x60> weightScale4x4; // 0x1C0
        std::array<u8, 0x80> weightScale8x8; // 0x220
        std::array<u8, 2> numInterViewRefs; // 0x2A0
        std::array<u8, 14> _pad2_; // 0x2A2
        std::array<std::array<i8, 16>, 2> interViewRefIdx; // 0x2B0
        union {
            u32 rawFlags2; // 0x2D0
            struct {
                u32 losslessIpred8x8FilterEnable : 1;
                u32 qpprimeYZeroTransformBypassFlag : 1;
                u32 _pad3_ : 30;
            };
        };
        std::array<u32, 7> displayParam; // 0x2D4
        std::array<u32, 3> _pad4_; // 0x2F0
    };
    static_assert(sizeof(H264DecoderContext) == 0x2FC);
    static_assert(offsetof(H264DecoderContext, streamLength) == 0x48);
    static_assert(offsetof(H264DecoderContext, parameterSet) == 0x58);
    static_assert(offsetof(H264DecoderContext, dpb) == 0xC0);
    static_assert(offsetof(H264DecoderContext, weightScale4x4) == 0x1C0);

    #pragma pack(pop)
}
