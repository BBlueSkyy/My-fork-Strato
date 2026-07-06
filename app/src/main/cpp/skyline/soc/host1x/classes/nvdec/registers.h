// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief The codec an NVDEC instance is configured to decode
     */
    enum class CodecId : u64 {
        None = 0x0,
        H264 = 0x3,
        Vp8 = 0x5,
        H265 = 0x7,
        Vp9 = 0x9,
    };

    /**
     * @brief A 256-byte aligned IOVA into the SMMU address space as held in NVDEC registers
     * @note NVDEC uses a 32-bit address space but its register slots are 64-bit wide
     */
    struct RegisterOffset {
        u64 raw;

        constexpr u64 Address() const {
            return raw << 8;
        }
    };
    static_assert(sizeof(RegisterOffset) == sizeof(u64));

    constexpr static size_t RegisterCount{0x178}; //!< The number of 64-bit method slots in the NVDEC method space

    /**
     * @brief The NVDEC register file, method indices map directly to 64-bit slots
     * @url https://github.com/NVIDIA/open-gpu-doc (clc5b0 video decoder class)
     */
    #pragma pack(push, 1)
    union Registers {
        std::array<u64, RegisterCount> raw;

        struct {
            std::array<u64, 0x80> _pad0_;
            CodecId codecId; // 0x80
            std::array<u64, 0x3F> _pad1_;
            u64 execute; // 0xC0
            std::array<u64, 0x3F> _pad2_;
            u64 controlParams; // 0x100
            RegisterOffset pictureInfoOffset; // 0x101
            RegisterOffset frameBitstreamOffset; // 0x102
            u64 frameNumber; // 0x103
            RegisterOffset h264SliceDataOffsets; // 0x104
            RegisterOffset h264MvDumpOffset; // 0x105
            std::array<u64, 3> _pad3_;
            RegisterOffset frameStatsOffset; // 0x109
            RegisterOffset h264LastSurfaceLumaOffset; // 0x10A
            RegisterOffset h264LastSurfaceChromaOffset; // 0x10B
            std::array<RegisterOffset, 17> surfaceLumaOffsets; // 0x10C
            std::array<RegisterOffset, 17> surfaceChromaOffsets; // 0x11D
            RegisterOffset picScratchBufOffset; // 0x12E
            RegisterOffset externalMvBufferOffset; // 0x12F
            std::array<u64, 16> _pad4_;
            RegisterOffset h264MbhistBufferOffset; // 0x140
            std::array<u64, 15> _pad5_;
            RegisterOffset vp8ProbDataOffset; // 0x150
            RegisterOffset vp8HeaderPartitionBufOffset; // 0x151
            std::array<u64, 14> _pad6_;
            RegisterOffset hevcScalingListOffset; // 0x160
            RegisterOffset hevcTileSizesOffset; // 0x161
            RegisterOffset hevcFilterBufferOffset; // 0x162
            RegisterOffset hevcSaoBufferOffset; // 0x163
            RegisterOffset hevcSliceInfoBufferOffset; // 0x164
            RegisterOffset hevcSliceGroupIndexOffset; // 0x165
            std::array<u64, 10> _pad7_;
            RegisterOffset vp9ProbTabBufferOffset; // 0x170
            RegisterOffset vp9CtxCounterBufferOffset; // 0x171
            RegisterOffset vp9SegmentReadBufferOffset; // 0x172
            RegisterOffset vp9SegmentWriteBufferOffset; // 0x173
            RegisterOffset vp9TileSizeBufferOffset; // 0x174
            RegisterOffset vp9ColMvWriteBufferOffset; // 0x175
            RegisterOffset vp9ColMvReadBufferOffset; // 0x176
            RegisterOffset vp9FilterBufferOffset; // 0x177
        };
    };
    #pragma pack(pop)
    static_assert(offsetof(Registers, codecId) == 0x80 * sizeof(u64));
    static_assert(offsetof(Registers, execute) == 0xC0 * sizeof(u64));
    static_assert(offsetof(Registers, controlParams) == 0x100 * sizeof(u64));
    static_assert(offsetof(Registers, surfaceLumaOffsets) == 0x10C * sizeof(u64));
    static_assert(offsetof(Registers, surfaceChromaOffsets) == 0x11D * sizeof(u64));
    static_assert(offsetof(Registers, vp8ProbDataOffset) == 0x150 * sizeof(u64));
    static_assert(offsetof(Registers, vp9ProbTabBufferOffset) == 0x170 * sizeof(u64));
    static_assert(sizeof(Registers) == RegisterCount * sizeof(u64));
}
