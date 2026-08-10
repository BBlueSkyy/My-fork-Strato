// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "codec.h"

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief The reference surface slots used by the VP8 hardware decoder within the surface offset register arrays
     */
    enum class Vp8SurfaceIndex : u32 {
        Last = 0,
        Golden = 1,
        AltRef = 2,
        Current = 3,
    };

    #pragma pack(push, 1)

    /**
     * @brief The VP8 picture information structure the guest driver writes for every decode operation
     * @url https://developer.nvidia.com/embedded/downloads (L4T multimedia nvdec_drv.h, nvdec_vp8_pic_s)
     */
    struct Vp8PictureInfo {
        std::array<u32, 14> _pad0_; // 0x00
        u16 frameWidth; // 0x38
        u16 frameHeight; // 0x3A
        u8 keyFrame; // 0x3C
        u8 version; // 0x3D
        union {
            u8 rawSurfaceFormat; // 0x3E
            struct {
                u8 tileFormat : 2;
                u8 gobHeight : 3;
                u8 _pad1_ : 3;
            };
        };
        u8 errorConcealOn; // 0x3F
        u32 firstPartSize; //!< The size of the first partition (frame header and macroblock header partition) // 0x40
        u32 histBufferSize; //!< In units of 256 // 0x44
        u32 vldBufferSize; //!< In units of 1 // 0x48
        std::array<u32, 2> frameStride; // 0x4C
        u32 lumaTopOffset; // 0x54
        u32 lumaBotOffset; // 0x58
        u32 lumaFrameOffset; // 0x5C
        u32 chromaTopOffset; // 0x60
        u32 chromaBotOffset; // 0x64
        u32 chromaFrameOffset; // 0x68
        std::array<u8, 0x1C> _pad2_; //!< Display parameters // 0x6C
        i8 currentOutputMemoryLayout; // 0x88
        std::array<i8, 3> outputMemoryLayout; //!< The NV12/NV24 setting of the golden/altref/last reference surfaces // 0x89
        u8 segmentationFeatureDataUpdate; // 0x8C
        std::array<u8, 3> _pad3_; // 0x8D
        u32 resultValue; //!< The microcode result of the decode operation // 0x90
        std::array<u32, 8> partitionOffset; // 0x94
        std::array<u32, 3> _pad4_; // 0xB4
    };
    static_assert(sizeof(Vp8PictureInfo) == 0xC0);
    static_assert(offsetof(Vp8PictureInfo, frameWidth) == 0x38);
    static_assert(offsetof(Vp8PictureInfo, firstPartSize) == 0x40);
    static_assert(offsetof(Vp8PictureInfo, resultValue) == 0x90);

    #pragma pack(pop)

    /**
     * @brief The VP8 codec rebuilds the frame tag header from the guest picture information and prepends it to the guest frame data for FFmpeg
     * @url https://datatracker.ietf.org/doc/html/rfc6386 (clause 9.1 uncompressed data chunk)
     */
    class Vp8 final : public Codec {
      private:
        Vp8PictureInfo context{};
        std::vector<u8> frameScratch; //!< Holds the composed bitstream packet for the current decode operation

        span<const u8> ComposeBitstream() override;

        u64 GetOutputLumaAddress() override;

      public:
        Vp8(const DeviceState &state, const Registers &registers);
    };
}
