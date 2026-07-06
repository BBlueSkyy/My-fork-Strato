// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>

namespace skyline::soc::host1x::vic {
    /**
     * @brief The pixel format of a VIC input/output surface
     * @note A single underscore separates pixels, a double underscore separates planes and an _N suffix denotes chroma subsampling
     */
    enum class VideoPixelFormat : u32 {
        A8B8G8R8 = 31,
        A8R8G8B8 = 32,
        B8G8R8A8 = 33,
        R8G8B8A8 = 34,
        X8B8G8R8 = 35,
        X8R8G8B8 = 36,
        Y8__U8V8_N420 = 67,
        Y8__V8U8_N420 = 68, //!< NV12: a Y plane followed by an interleaved VU plane with 4:2:0 subsampling
    };

    /**
     * @brief The memory layout of a VIC surface
     */
    enum class BlkKind : u32 {
        Pitch = 0,
        Generic16Bx2 = 1, //!< Block-linear layout with 16Bx2 GOB sectors
    };

    /**
     * @brief A 256-byte aligned IOVA into the SMMU address space as held in VIC registers
     */
    struct RegisterOffset {
        u32 raw;

        constexpr u64 Address() const {
            return static_cast<u64>(raw) << 8;
        }
    };
    static_assert(sizeof(RegisterOffset) == sizeof(u32));

    /**
     * @brief The IOVAs of every plane of a surface
     */
    struct PlaneOffsets {
        RegisterOffset luma;
        RegisterOffset chromaU;
        RegisterOffset chromaV;
    };
    static_assert(sizeof(PlaneOffsets) == 0xC);

    /**
     * @brief Configures the pixel format, memory layout and dimensions of the VIC output surface
     */
    struct OutputSurfaceConfig {
        union {
            u32 raw0;
            struct {
                VideoPixelFormat outPixelFormat : 7;
                u32 outChromaLocHoriz : 2;
                u32 outChromaLocVert : 2;
                BlkKind outBlkKind : 4;
                u32 outBlkHeight : 4; //!< log2 of the number of GOBs per block
                u32 _pad0_ : 13;
            };
        };
        union {
            u32 raw1;
            struct {
                u32 outSurfaceWidth : 14; //!< Minus 1
                u32 outSurfaceHeight : 14; //!< Minus 1
                u32 _pad1_ : 4;
            };
        };
        union {
            u32 raw2;
            struct {
                u32 outLumaWidth : 14; //!< Minus 1
                u32 outLumaHeight : 14; //!< Minus 1
                u32 _pad2_ : 4;
            };
        };
        union {
            u32 raw3;
            struct {
                u32 outChromaWidth : 14; //!< Minus 1
                u32 outChromaHeight : 14; //!< Minus 1
                u32 _pad3_ : 4;
            };
        };
    };
    static_assert(sizeof(OutputSurfaceConfig) == 0x10);

    /**
     * @brief Configures the frame format and processing options of a single input slot
     */
    struct SlotConfig {
        union {
            u64 raw0;
            struct {
                u64 slotEnable : 1;
                u64 _pad0_ : 15;
                u64 frameFormat : 4; //!< DXVA HD frame format, 0 = progressive, 1/2 = top/bottom field
                u64 _pad1_ : 44;
            };
        };
        std::array<u64, 7> _unused_; //!< Denoise/deinterlace/alpha configuration which is irrelevant to progressive video frames
    };
    static_assert(sizeof(SlotConfig) == 0x40);

    /**
     * @brief Configures the pixel format, memory layout and dimensions of an input slot's surface
     */
    struct SlotSurfaceConfig {
        union {
            u32 raw0;
            struct {
                VideoPixelFormat slotPixelFormat : 7;
                u32 slotChromaLocHoriz : 2;
                u32 slotChromaLocVert : 2;
                u32 slotBlkKind : 4;
                u32 slotBlkHeight : 4; //!< log2 of the number of GOBs per block
                u32 slotCacheWidth : 3;
                u32 _pad0_ : 10;
            };
        };
        union {
            u32 raw1;
            struct {
                u32 slotSurfaceWidth : 14; //!< Minus 1
                u32 slotSurfaceHeight : 14; //!< Minus 1
                u32 _pad1_ : 4;
            };
        };
        union {
            u32 raw2;
            struct {
                u32 slotLumaWidth : 14; //!< Padded, minus 1
                u32 slotLumaHeight : 14; //!< Padded, minus 1
                u32 _pad2_ : 4;
            };
        };
        union {
            u32 raw3;
            struct {
                u32 slotChromaWidth : 14; //!< Padded, minus 1
                u32 slotChromaHeight : 14; //!< Padded, minus 1
                u32 _pad3_ : 4;
            };
        };
    };
    static_assert(sizeof(SlotSurfaceConfig) == 0x10);

    /**
     * @brief The aggregate configuration of a single VIC input slot
     */
    struct SlotStruct {
        SlotConfig config;
        SlotSurfaceConfig surfaceConfig;
        std::array<u64, 2> lumaKey; //!< Opaque as luma keying is unused for plain frame conversion
        std::array<u64, 4> colorMatrix;
        std::array<u64, 4> gamutMatrix;
        std::array<u64, 2> blending;
    };
    static_assert(sizeof(SlotStruct) == 0xB0);

    /**
     * @brief The VIC configuration structure supplied by the guest via SetConfigStructOffset
     * @url https://github.com/NVIDIA/open-gpu-doc (clb0b6 video compositor class)
     */
    struct ConfigStruct {
        std::array<u64, 2> pipeConfig;
        std::array<u64, 2> outputConfig;
        OutputSurfaceConfig outputSurfaceConfig;
        std::array<u64, 4> outColorMatrix;
        std::array<u32, 16> clearRects;
        std::array<SlotStruct, 8> slotStructs;
    };
    static_assert(offsetof(ConfigStruct, outputSurfaceConfig) == 0x20);
    static_assert(offsetof(ConfigStruct, slotStructs) == 0x90);
    static_assert(sizeof(ConfigStruct) == 0x610);
}
