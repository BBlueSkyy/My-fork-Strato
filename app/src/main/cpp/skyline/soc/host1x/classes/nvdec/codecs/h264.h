// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <soc/host1x/classes/nvdec/types.h>
#include "codec.h"

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief A bitstream writer for H.264 header syntax elements
     * @url https://www.itu.int/rec/T-REC-H.264 (clause 9.1 Exp-Golomb coding, 7.3.2 parameter set syntax)
     */
    class H264BitWriter {
      private:
        i32 buffer{}; //!< The bits accumulated for the current byte
        i32 bufferPosition{}; //!< The number of bits currently held in the buffer
        std::vector<u8> byteArray;

        /**
         * @brief Writes bitCount bits from value into the buffer
         */
        void WriteBits(i32 value, i32 bitCount);

        void WriteExpGolombCodedInt(i32 value);

        void WriteExpGolombCodedUInt(u32 value);

        /**
         * @brief Gets the next available position in the buffer, invoking Flush() if the buffer is full
         */
        i32 GetFreeBufferBits();

        /**
         * @brief Pushes the currently accumulated byte into the byte array and resets the buffer
         */
        void Flush();

      public:
        /**
         * @brief Writes an unsigned integer value
         */
        void WriteU(i32 value, i32 valueSize);

        /**
         * @brief Writes a signed value in the Exp-Golomb-coded syntax
         */
        void WriteSe(i32 value);

        /**
         * @brief Writes an unsigned value in the Exp-Golomb-coded syntax
         */
        void WriteUe(u32 value);

        /**
         * @brief Writes a single bit into the stream
         */
        void WriteBit(bool state);

        /**
         * @brief Writes a scaling matrix as delta-coded values in zig-zag scan order per clause 7.3.2.1.1.1
         */
        void WriteScalingList(span<const u8> list, i32 start, i32 count);

        /**
         * @brief Finalises the current NAL with an RBSP stop bit and byte alignment
         */
        void End();

        const std::vector<u8> &GetByteArray() const {
            return byteArray;
        }
    };

    /**
     * @brief The H.264 codec re-composes SPS/PPS headers from the guest picture parameters and prepends them to the guest slice data for FFmpeg
     */
    class H264 final : public Codec {
      private:
        H264DecoderContext context{};
        std::vector<u8> frameScratch; //!< Holds the composed bitstream packet for the current decode operation
        bool firstFrame{true};

        span<const u8> ComposeBitstream() override;

        u64 GetOutputLumaAddress() override;

      public:
        H264(const DeviceState &state, const Registers &registers);
    };
}
