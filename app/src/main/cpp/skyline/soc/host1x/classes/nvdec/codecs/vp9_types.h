// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <vector>
#include <common.h>

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief The reference surface slots used by the VP9 hardware decoder within the surface offset register arrays
     */
    enum class Vp9SurfaceIndex : u32 {
        Last = 0,
        Golden = 1,
        AltRef = 2,
        Current = 3,
    };

    #pragma pack(push, 1)

    /**
     * @brief The dimensions and pitches of a single VP9 frame surface
     */
    struct Vp9FrameDimensions {
        i16 width;
        i16 height;
        i16 lumaPitch;
        i16 chromaPitch;
    };
    static_assert(sizeof(Vp9FrameDimensions) == 0x8);

    /**
     * @brief Frame-level state flags the guest driver supplies for a VP9 decode operation
     */
    union Vp9FrameFlags {
        u32 raw;
        struct {
            u32 isKeyFrame : 1;
            u32 lastFrameIsKeyFrame : 1;
            u32 frameSizeChanged : 1;
            u32 errorResilientMode : 1;
            u32 lastShowFrame : 1;
            u32 intraOnly : 1;
            u32 _pad0_ : 26;
        };
    };
    static_assert(sizeof(Vp9FrameFlags) == 0x4);

    /**
     * @brief The VP9 segmentation feature state
     * @url https://www.webmproject.org/vp9/ (VP9 Bitstream & Decoding Process Specification, clause 6.2.11 segmentation_params)
     */
    struct Segmentation {
        u8 enabled;
        u8 updateMap;
        u8 temporalUpdate;
        u8 absDelta;
        std::array<std::array<u8, 4>, 8> featureEnabled;
        std::array<std::array<i16, 4>, 8> featureData;

        bool operator==(const Segmentation &) const = default;
    };
    static_assert(sizeof(Segmentation) == 0x64);

    /**
     * @brief The VP9 loop filter delta state
     * @url https://www.webmproject.org/vp9/ (VP9 Bitstream & Decoding Process Specification, clause 6.2.10 loop_filter_params)
     */
    struct LoopFilter {
        u8 modeRefDeltaEnabled;
        std::array<i8, 4> refDeltas;
        std::array<i8, 2> modeDeltas;
    };
    static_assert(sizeof(LoopFilter) == 0x7);

    /**
     * @brief The probability tables of a single VP9 frame context arranged for compressed header composition
     * @url https://www.webmproject.org/vp9/ (VP9 Bitstream & Decoding Process Specification, clause 6.3 compressed header semantics)
     */
    struct Vp9EntropyProbs {
        std::array<u8, 36> yModeProb; // 0x0
        std::array<u8, 64> partitionProb; // 0x24
        std::array<u8, 1728> coefProbs; // 0x64
        std::array<u8, 8> switchableInterpProb; // 0x724
        std::array<u8, 28> interModeProb; // 0x72C
        std::array<u8, 4> intraInterProb; // 0x748
        std::array<u8, 5> compInterProb; // 0x74C
        std::array<u8, 10> singleRefProb; // 0x751
        std::array<u8, 5> compRefProb; // 0x75B
        std::array<u8, 6> tx32x32Prob; // 0x760
        std::array<u8, 4> tx16x16Prob; // 0x766
        std::array<u8, 2> tx8x8Prob; // 0x76A
        std::array<u8, 3> skipProbs; // 0x76C
        std::array<u8, 3> joints; // 0x76F
        std::array<u8, 2> sign; // 0x772
        std::array<u8, 20> classes; // 0x774
        std::array<u8, 2> class0; // 0x788
        std::array<u8, 20> probBits; // 0x78A
        std::array<u8, 12> class0Fr; // 0x79E
        std::array<u8, 6> fr; // 0x7AA
        std::array<u8, 2> class0Hp; // 0x7B0
        std::array<u8, 2> highPrecision; // 0x7B2
    };
    static_assert(sizeof(Vp9EntropyProbs) == 0x7B4);
    static_assert(offsetof(Vp9EntropyProbs, partitionProb) == 0x24);
    static_assert(offsetof(Vp9EntropyProbs, switchableInterpProb) == 0x724);
    static_assert(offsetof(Vp9EntropyProbs, sign) == 0x772);
    static_assert(offsetof(Vp9EntropyProbs, class0Fr) == 0x79E);
    static_assert(offsetof(Vp9EntropyProbs, highPrecision) == 0x7B2);

    #pragma pack(pop)

    /**
     * @brief Deserialised VP9 picture information for a single decode operation, used to drive header composition
     */
    struct Vp9PictureInfo {
        u32 bitstreamSize;
        std::array<u64, 4> frameOffsets; //!< The luma surface IOVAs of the last/golden/altref/current reference slots
        std::array<i8, 4> refFrameSignBias;
        i32 baseQIndex;
        i32 yDcDeltaQ;
        i32 uvDcDeltaQ;
        i32 uvAcDeltaQ;
        i32 transformMode;
        i32 interpFilter;
        i32 referenceMode;
        i32 log2TileCols;
        i32 log2TileRows;
        std::array<i8, 4> refDeltas;
        std::array<i8, 2> modeDeltas;
        Vp9EntropyProbs entropy;
        Vp9FrameDimensions frameSize;
        u8 firstLevel;
        u8 sharpnessLevel;
        bool isKeyFrame;
        bool intraOnly;
        bool lastFrameWasKey;
        bool errorResilientMode;
        bool lastFrameShown;
        bool showFrame;
        bool lossless;
        bool allowHighPrecisionMv;
        bool segmentEnabled;
        bool modeRefDeltaEnabled;
    };

    /**
     * @brief A buffered VP9 frame alongside its picture information, decode submission runs one frame behind guest submission to allow show_frame patching
     */
    struct Vp9FrameContainer {
        Vp9PictureInfo info{};
        std::vector<u8> bitstream;
    };

    #pragma pack(push, 1)

    /**
     * @brief The VP9 picture information structure the guest driver writes for every decode operation
     * @url https://developer.nvidia.com/embedded/downloads (L4T multimedia nvdec_drv.h, nvdec_vp9_pic_s)
     */
    struct Vp9GuestPictureInfo {
        std::array<u32, 12> _pad0_; // 0x00
        u32 bitstreamSize; // 0x30
        std::array<u32, 5> _pad1_; // 0x34
        Vp9FrameDimensions lastFrameSize; // 0x48
        Vp9FrameDimensions goldenFrameSize; // 0x50
        Vp9FrameDimensions altFrameSize; // 0x58
        Vp9FrameDimensions currentFrameSize; // 0x60
        Vp9FrameFlags vp9Flags; // 0x68
        std::array<i8, 4> refFrameSignBias; // 0x6C
        u8 firstLevel; // 0x70
        u8 sharpnessLevel; // 0x71
        u8 baseQIndex; // 0x72
        u8 yDcDeltaQ; // 0x73
        u8 uvAcDeltaQ; // 0x74
        u8 uvDcDeltaQ; // 0x75
        u8 lossless; // 0x76
        u8 txMode; // 0x77
        u8 allowHighPrecisionMv; // 0x78
        u8 interpFilter; // 0x79
        u8 referenceMode; // 0x7A
        std::array<u8, 3> _pad2_; // 0x7B
        u8 log2TileCols; // 0x7E
        u8 log2TileRows; // 0x7F
        Segmentation segmentation; // 0x80
        LoopFilter loopFilter; // 0xE4
        std::array<u8, 21> _pad3_; // 0xEB

        Vp9PictureInfo Convert() const {
            return {
                .bitstreamSize = bitstreamSize,
                .frameOffsets{},
                .refFrameSignBias = refFrameSignBias,
                .baseQIndex = baseQIndex,
                .yDcDeltaQ = yDcDeltaQ,
                .uvDcDeltaQ = uvDcDeltaQ,
                .uvAcDeltaQ = uvAcDeltaQ,
                .transformMode = txMode,
                .interpFilter = interpFilter,
                .referenceMode = referenceMode,
                .log2TileCols = log2TileCols,
                .log2TileRows = log2TileRows,
                .refDeltas = loopFilter.refDeltas,
                .modeDeltas = loopFilter.modeDeltas,
                .entropy{},
                .frameSize = currentFrameSize,
                .firstLevel = firstLevel,
                .sharpnessLevel = sharpnessLevel,
                .isKeyFrame = vp9Flags.isKeyFrame != 0,
                .intraOnly = vp9Flags.intraOnly != 0,
                .lastFrameWasKey = vp9Flags.lastFrameIsKeyFrame != 0,
                .errorResilientMode = vp9Flags.errorResilientMode != 0,
                .lastFrameShown = vp9Flags.lastShowFrame != 0,
                .showFrame = true,
                .lossless = lossless != 0,
                .allowHighPrecisionMv = allowHighPrecisionMv != 0,
                .segmentEnabled = segmentation.enabled != 0,
                .modeRefDeltaEnabled = loopFilter.modeRefDeltaEnabled != 0,
            };
        }
    };
    static_assert(sizeof(Vp9GuestPictureInfo) == 0x100);
    static_assert(offsetof(Vp9GuestPictureInfo, bitstreamSize) == 0x30);
    static_assert(offsetof(Vp9GuestPictureInfo, lastFrameSize) == 0x48);
    static_assert(offsetof(Vp9GuestPictureInfo, firstLevel) == 0x70);
    static_assert(offsetof(Vp9GuestPictureInfo, segmentation) == 0x80);
    static_assert(offsetof(Vp9GuestPictureInfo, loopFilter) == 0xE4);

    /**
     * @brief The VP9 entropy probability tables in the layout the guest driver writes to the probability tab buffer
     * @url https://developer.nvidia.com/embedded/downloads (L4T multimedia nvdec_drv.h, nvdec_vp9_prob_tab_s)
     */
    struct Vp9GuestEntropyProbs {
        std::array<u8, 10 * 10 * 8> kfBmodeProb; // 0x0
        std::array<u8, 10 * 10 * 1> kfBmodeProbB; // 0x320
        std::array<u8, 3> refPredProbs; // 0x384
        std::array<u8, 7> mbSegmentTreeProbs; // 0x387
        std::array<u8, 3> segmentPredProbs; // 0x38E
        std::array<u8, 4> refScores; // 0x391
        std::array<u8, 2> probCompPred; // 0x395
        std::array<u8, 9> _pad0_; // 0x397
        std::array<u8, 10 * 8> kfUvModeProb; // 0x3A0
        std::array<u8, 10 * 1> kfUvModeProbB; // 0x3F0
        std::array<u8, 6> _pad1_; // 0x3FA
        std::array<u8, 28> interModeProb; // 0x400
        std::array<u8, 4> intraInterProb; // 0x41C
        std::array<u8, 80> _pad2_; // 0x420
        std::array<u8, 2> tx8x8Prob; // 0x470
        std::array<u8, 4> tx16x16Prob; // 0x472
        std::array<u8, 6> tx32x32Prob; // 0x476
        std::array<u8, 4> yModeProbE8; // 0x47C
        std::array<std::array<u8, 8>, 4> yModeProbE0E7; // 0x480
        std::array<u8, 64> _pad3_; // 0x4A0
        std::array<u8, 64> partitionProb; // 0x4E0
        std::array<u8, 10> _pad4_; // 0x520
        std::array<u8, 8> switchableInterpProb; // 0x52A
        std::array<u8, 5> compInterProb; // 0x532
        std::array<u8, 3> skipProbs; // 0x537
        std::array<u8, 1> _pad5_; // 0x53A
        std::array<u8, 3> joints; // 0x53B
        std::array<u8, 2> sign; // 0x53E
        std::array<u8, 2> class0; // 0x540
        std::array<u8, 6> fr; // 0x542
        std::array<u8, 2> class0Hp; // 0x548
        std::array<u8, 2> highPrecision; // 0x54A
        std::array<u8, 20> classes; // 0x54C
        std::array<u8, 12> class0Fr; // 0x560
        std::array<u8, 20> predBits; // 0x56C
        std::array<u8, 10> singleRefProb; // 0x580
        std::array<u8, 5> compRefProb; // 0x58A
        std::array<u8, 17> _pad6_; // 0x58F
        std::array<u8, 2304> coefProbs; // 0x5A0

        void Convert(Vp9EntropyProbs &fc) {
            fc.interModeProb = interModeProb;
            fc.intraInterProb = intraInterProb;
            fc.tx8x8Prob = tx8x8Prob;
            fc.tx16x16Prob = tx16x16Prob;
            fc.tx32x32Prob = tx32x32Prob;

            for (size_t i{}; i < 4; i++)
                for (size_t j{}; j < 9; j++)
                    fc.yModeProb[j + 9 * i] = j < 8 ? yModeProbE0E7[i][j] : yModeProbE8[i];

            fc.partitionProb = partitionProb;
            fc.switchableInterpProb = switchableInterpProb;
            fc.compInterProb = compInterProb;
            fc.skipProbs = skipProbs;
            fc.joints = joints;
            fc.sign = sign;
            fc.class0 = class0;
            fc.fr = fr;
            fc.class0Hp = class0Hp;
            fc.highPrecision = highPrecision;
            fc.classes = classes;
            fc.class0Fr = class0Fr;
            fc.probBits = predBits;
            fc.singleRefProb = singleRefProb;
            fc.compRefProb = compRefProb;

            // Skip the 4th element of every table entry as it goes unused
            for (size_t i{}; i < coefProbs.size(); i += 4) {
                const size_t j{i - i / 4};
                fc.coefProbs[j] = coefProbs[i];
                fc.coefProbs[j + 1] = coefProbs[i + 1];
                fc.coefProbs[j + 2] = coefProbs[i + 2];
            }
        }
    };
    static_assert(sizeof(Vp9GuestEntropyProbs) == 0xEA0);
    static_assert(offsetof(Vp9GuestEntropyProbs, interModeProb) == 0x400);
    static_assert(offsetof(Vp9GuestEntropyProbs, tx8x8Prob) == 0x470);
    static_assert(offsetof(Vp9GuestEntropyProbs, partitionProb) == 0x4E0);
    static_assert(offsetof(Vp9GuestEntropyProbs, class0) == 0x540);
    static_assert(offsetof(Vp9GuestEntropyProbs, class0Fr) == 0x560);
    static_assert(offsetof(Vp9GuestEntropyProbs, coefProbs) == 0x5A0);

    #pragma pack(pop)
}
