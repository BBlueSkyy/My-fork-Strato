// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "vp9_types.h"
#include "codec.h"

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief A boolean range encoder (arithmetic coder) used to compose the VP9 compressed header
     * @url https://www.webmproject.org/vp9/ (VP9 Bitstream & Decoding Process Specification, clause 9.2 bool decoding process)
     */
    class VpxRangeEncoder {
      private:
        std::vector<u8> buffer; //!< The encoded bytes composed so far
        u32 lowValue{}; //!< The lower end of the current arithmetic coding interval
        u32 range{0xFF}; //!< The current range of the arithmetic coder, kept normalised to eight bits
        i32 count{-24}; //!< The number of bits accumulated towards the next output byte, a byte is emitted once this becomes non-negative; starting at -24 delays the first byte until 24 bits have been shifted in

        constexpr static i32 HalfProbability{128};

      public:
        VpxRangeEncoder();

        /**
         * @brief Writes the rightmost valueSize bits from value into the stream
         */
        void Write(i32 value, i32 valueSize);

        /**
         * @brief Writes a single bit with half probability
         */
        void Write(bool bit);

        /**
         * @brief Writes a bit into the stream encoded with the supplied probability
         */
        void Write(bool bit, i32 probability);

        /**
         * @brief Signals the end of the bitstream
         */
        void End();

        std::vector<u8> &GetBuffer() {
            return buffer;
        }

        const std::vector<u8> &GetBuffer() const {
            return buffer;
        }
    };

    /**
     * @brief A bitstream writer for the VP9 uncompressed header syntax elements
     * @url https://www.webmproject.org/vp9/ (VP9 Bitstream & Decoding Process Specification, clause 6.2 uncompressed header syntax)
     */
    class VpxBitStreamWriter {
      private:
        i32 buffer{}; //!< The bits accumulated for the current byte
        i32 bufferPosition{}; //!< The number of bits currently held in the buffer
        std::vector<u8> byteArray;

        /**
         * @brief Writes bitCount bits from value into the buffer
         */
        void WriteBits(u32 value, u32 bitCount);

        /**
         * @brief Gets the next available position in the buffer, invoking Flush() if the buffer is full
         */
        i32 GetFreeBufferBits();

      public:
        /**
         * @brief Writes an unsigned integer value
         */
        void WriteU(u32 value, u32 valueSize);

        /**
         * @brief Writes a signed integer value as its magnitude followed by a sign bit
         */
        void WriteS(i32 value, u32 valueSize);

        /**
         * @brief Writes a delta coded quantiser value per clause 6.2.10 of the VP9 specification
         */
        void WriteDeltaQ(u32 value);

        void WriteBit(bool state);

        /**
         * @brief Pushes the currently accumulated byte into the byte array and resets the buffer
         */
        void Flush();

        std::vector<u8> &GetByteArray() {
            return byteArray;
        }

        const std::vector<u8> &GetByteArray() const {
            return byteArray;
        }
    };

    /**
     * @brief The VP9 codec re-composes the uncompressed and compressed frame headers from the guest picture information and prepends them to the guest tile bitstream for FFmpeg
     */
    class Vp9 final : public Codec {
      private:
        std::array<i8, 4> loopFilterRefDeltas{};
        std::array<i8, 2> loopFilterModeDeltas{};
        std::vector<u8> frameScratch; //!< Holds the composed bitstream packet for the current decode operation
        Vp9FrameContainer nextFrame{}; //!< The buffered upcoming frame, decode submission runs one frame behind the guest so show_frame can be patched from the following frame's information
        std::array<Vp9EntropyProbs, 4> frameContexts{}; //!< The four VP9 frame contexts of entropy probabilities maintained across frames
        bool swapRefIndices{}; //!< If the golden and altref reference indices are currently swapped relative to the guest's surface slots
        Segmentation lastSegmentation{}; //!< The segmentation parameters of the previous frame, used to detect when the guest changes them
        Vp9GuestPictureInfo currentPictureInfo{};
        Vp9PictureInfo currentFrameInfo{};
        Vp9EntropyProbs prevFrameProbs{}; //!< The probabilities of the previous frame, compressed header updates are encoded as deltas against these

        /**
         * @brief Writes compressed header probability updates for an array of probabilities
         */
        template<typename T, size_t N>
        void WriteProbabilityUpdate(VpxRangeEncoder &writer, const std::array<T, N> &newProb, const std::array<T, N> &oldProb);

        /**
         * @brief Writes a compressed header probability update, if the probabilities differ WriteProbabilityDelta is invoked
         */
        void WriteProbabilityUpdate(VpxRangeEncoder &writer, u8 newProb, u8 oldProb);

        /**
         * @brief Writes a compressed header probability delta
         */
        void WriteProbabilityDelta(VpxRangeEncoder &writer, u8 newProb, u8 oldProb);

        /**
         * @brief Inverse of clause 6.3.4 (decode_term_subexp) of the VP9 specification
         */
        void EncodeTermSubExp(VpxRangeEncoder &writer, i32 value);

        /**
         * @brief Writes if the value is less than the test value
         */
        bool WriteLessThan(VpxRangeEncoder &writer, i32 value, i32 test);

        /**
         * @brief Writes probability updates for the coefficient probabilities
         */
        void WriteCoefProbabilityUpdate(VpxRangeEncoder &writer, i32 txMode, const std::array<u8, 1728> &newProb, const std::array<u8, 1728> &oldProb);

        /**
         * @brief Writes probability updates for 4-byte aligned probability tables, skipping every 4th (unused) entry
         */
        template<typename T, size_t N>
        void WriteProbabilityUpdateAligned4(VpxRangeEncoder &writer, const std::array<T, N> &newProb, const std::array<T, N> &oldProb);

        /**
         * @brief Writes a motion vector probability update per clause 6.3.17 of the VP9 specification
         */
        void WriteMvProbabilityUpdate(VpxRangeEncoder &writer, u8 newProb, u8 oldProb);

        /**
         * @brief Writes the segmentation parameters into the uncompressed header
         */
        void WriteSegmentation(VpxBitStreamWriter &writer);

        /**
         * @brief Reads and deserialises the guest supplied VP9 picture information for the current decode operation
         */
        Vp9PictureInfo GetVp9PictureInfo();

        /**
         * @brief Reads and converts the guest supplied entropy probabilities into a Vp9EntropyProbs structure
         */
        void InsertEntropy(u64 offset, Vp9EntropyProbs &dst);

        /**
         * @brief Returns the frame to be decoded after buffering, one frame is buffered so show_frame can be patched from the following frame's information
         */
        Vp9FrameContainer GetCurrentFrame();

        /**
         * @brief Composes the compressed header from the guest picture information
         */
        std::vector<u8> ComposeCompressedHeader();

        /**
         * @brief Composes the uncompressed header from the guest picture information
         * @note This sets the previous frame probabilities which the compressed header composition requires
         */
        VpxBitStreamWriter ComposeUncompressedHeader();

        span<const u8> ComposeBitstream() override;

        u64 GetOutputLumaAddress() override;

      public:
        Vp9(const DeviceState &state, const Registers &registers);
    };
}
