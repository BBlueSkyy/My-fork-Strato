// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include <soc.h>
#include "vp8.h"

namespace skyline::soc::host1x::nvdec {
    Vp8::Vp8(const DeviceState &state, const Registers &registers) : Codec(state, registers) {
        initialized = decoder.Initialize(CodecId::Vp8);
    }

    u64 Vp8::GetOutputLumaAddress() {
        return registers.surfaceLumaOffsets[static_cast<size_t>(Vp8SurfaceIndex::Current)].Address();
    }

    span<const u8> Vp8::ComposeBitstream() {
        context = state.soc->smmu.Read<Vp8PictureInfo>(static_cast<u32>(registers.pictureInfoOffset.Address()));

        if (!registers.frameBitstreamOffset.raw || !context.vldBufferSize || context.vldBufferSize > MaxBitstreamSize) {
            LOGW("Invalid VP8 bitstream, offset: 0x{:X}, size: 0x{:X}", registers.frameBitstreamOffset.Address(), context.vldBufferSize);
            return {};
        }

        const bool isKeyFrame{context.keyFrame == 1};
        const size_t bitstreamSize{context.vldBufferSize};
        const size_t headerSize{isKeyFrame ? 10U : 3U};
        frameScratch.resize(headerSize + bitstreamSize);

        // Rebuild the frame tag as per clause 9.1 of RFC 6386
        frameScratch[0] = isKeyFrame ? 0 : 1; // 1-bit frame type (0: key frame, 1: interframe)
        frameScratch[0] |= static_cast<u8>((context.version & 7) << 1); // 3-bit version number
        frameScratch[0] |= static_cast<u8>(1 << 4); // 1-bit show_frame flag

        // The next 19 bits hold the first partition size
        frameScratch[0] |= static_cast<u8>((context.firstPartSize & 0x7) << 5);
        frameScratch[1] = static_cast<u8>((context.firstPartSize & 0x7F8) >> 3);
        frameScratch[2] = static_cast<u8>((context.firstPartSize & 0x7F800) >> 11);

        if (isKeyFrame) {
            // Start code
            frameScratch[3] = 0x9D;
            frameScratch[4] = 0x01;
            frameScratch[5] = 0x2A;
            // 16 bits: (2-bit horizontal scale << 14) | 14-bit width
            frameScratch[6] = static_cast<u8>(context.frameWidth & 0xFF);
            frameScratch[7] = static_cast<u8>((context.frameWidth >> 8) & 0x3F);
            // 16 bits: (2-bit vertical scale << 14) | 14-bit height
            frameScratch[8] = static_cast<u8>(context.frameHeight & 0xFF);
            frameScratch[9] = static_cast<u8>((context.frameHeight >> 8) & 0x3F);
        }

        state.soc->smmu.Read(span<u8>(frameScratch.data() + headerSize, bitstreamSize), static_cast<u32>(registers.frameBitstreamOffset.Address()));

        return frameScratch;
    }
}
