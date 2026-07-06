// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include <bit>
#include <soc.h>
#include "h264.h"

namespace skyline::soc::host1x::nvdec {
    H264::H264(const DeviceState &state, const Registers &registers) : Codec(state, registers) {
        initialized = decoder.Initialize(CodecId::H264);
    }

    u64 H264::GetOutputLumaAddress() {
        return registers.surfaceLumaOffsets[context.parameterSet.currPicIdx].Address() + context.parameterSet.lumaFrameOffset.Address();
    }

    span<const u8> H264::ComposeBitstream() {
        context = state.soc->smmu.Read<H264DecoderContext>(static_cast<u32>(registers.pictureInfoOffset.Address()));

        if (!registers.frameBitstreamOffset.raw || context.streamLength > MaxBitstreamSize) {
            LOGW("Invalid bitstream, offset: 0x{:X}, length: 0x{:X}", registers.frameBitstreamOffset.Address(), context.streamLength);
            return {};
        }

        if (context.parameterSet.fieldPic || context.parameterSet.lumaTopOffset.raw || context.parameterSet.lumaBotOffset.raw)
            LOGW("Interlaced H264 content is unsupported, output will be treated as progressive");

        // Frames after the first IDR frame carry their own headers within the guest slice data
        if (!firstFrame && context.parameterSet.frameNumber != 0) {
            frameScratch.resize(context.streamLength);
            state.soc->smmu.Read(span<u8>(frameScratch), static_cast<u32>(registers.frameBitstreamOffset.Address()));
            return frameScratch;
        }

        firstFrame = false;

        auto &parameterSet{context.parameterSet};
        H264BitWriter writer;

        // SPS (clause 7.3.2.1)
        writer.WriteU(1, 24); // Annex B start code
        writer.WriteU(0, 1); // forbidden_zero_bit
        writer.WriteU(3, 2); // nal_ref_idc
        writer.WriteU(7, 5); // nal_unit_type (SPS)
        writer.WriteU(100, 8); // profile_idc (High)
        writer.WriteU(0, 8); // constraint flags
        writer.WriteU(31, 8); // level_idc
        writer.WriteUe(0); // seq_parameter_set_id

        u32 chromaFormatIdc{static_cast<u32>(parameterSet.chromaFormatIdc)};
        writer.WriteUe(chromaFormatIdc);
        if (chromaFormatIdc == 3)
            writer.WriteBit(false); // separate_colour_plane_flag

        writer.WriteUe(0); // bit_depth_luma_minus8
        writer.WriteUe(0); // bit_depth_chroma_minus8
        writer.WriteBit(context.qpprimeYZeroTransformBypassFlag != 0);
        writer.WriteBit(false); // seq_scaling_matrix_present_flag

        writer.WriteUe(static_cast<u32>(parameterSet.log2MaxFrameNumMinus4));

        u32 picOrderCntType{static_cast<u32>(parameterSet.picOrderCntType)};
        writer.WriteUe(picOrderCntType);
        if (picOrderCntType == 0) {
            writer.WriteUe(static_cast<u32>(parameterSet.log2MaxPicOrderCntLsbMinus4));
        } else if (picOrderCntType == 1) {
            writer.WriteBit(parameterSet.deltaPicOrderAlwaysZeroFlag != 0);
            writer.WriteSe(0); // offset_for_non_ref_pic
            writer.WriteSe(0); // offset_for_top_to_bottom_field
            writer.WriteUe(0); // num_ref_frames_in_pic_order_cnt_cycle
        }

        i32 picHeightInMbs{static_cast<i32>(parameterSet.frameHeightInMbs) / (parameterSet.frameMbsOnlyFlag ? 1 : 2)};
        u32 maxNumRefFrames{static_cast<u32>(std::max(std::max(parameterSet.numRefIdxL0DefaultActive, parameterSet.numRefIdxL1DefaultActive) + 1, 4))};

        writer.WriteUe(maxNumRefFrames);
        writer.WriteBit(false); // gaps_in_frame_num_value_allowed_flag
        writer.WriteUe(parameterSet.picWidthInMbs - 1);
        writer.WriteUe(static_cast<u32>(picHeightInMbs - 1));
        writer.WriteBit(parameterSet.frameMbsOnlyFlag != 0);

        if (!parameterSet.frameMbsOnlyFlag)
            writer.WriteBit(parameterSet.mbaffFrame != 0);

        writer.WriteBit(parameterSet.direct8x8Inference != 0);
        writer.WriteBit(false); // frame_cropping_flag
        writer.WriteBit(false); // vui_parameters_present_flag
        writer.End();

        // PPS (clause 7.3.2.2)
        writer.WriteU(1, 24); // Annex B start code
        writer.WriteU(0, 1); // forbidden_zero_bit
        writer.WriteU(3, 2); // nal_ref_idc
        writer.WriteU(8, 5); // nal_unit_type (PPS)
        writer.WriteUe(0); // pic_parameter_set_id
        writer.WriteUe(0); // seq_parameter_set_id

        writer.WriteBit(parameterSet.entropyCodingModeFlag != 0);
        writer.WriteBit(parameterSet.picOrderPresentFlag != 0);
        writer.WriteUe(0); // num_slice_groups_minus1
        writer.WriteUe(static_cast<u32>(parameterSet.numRefIdxL0DefaultActive));
        writer.WriteUe(static_cast<u32>(parameterSet.numRefIdxL1DefaultActive));
        writer.WriteBit(parameterSet.weightedPred != 0);
        writer.WriteU(static_cast<i32>(parameterSet.weightedBipredIdc), 2);
        writer.WriteSe(static_cast<i32>(parameterSet.picInitQpMinus26));
        writer.WriteSe(0); // pic_init_qs_minus26
        writer.WriteSe(static_cast<i32>(parameterSet.chromaQpIndexOffset));
        writer.WriteBit(parameterSet.deblockingFilterControlPresentFlag != 0);
        writer.WriteBit(parameterSet.constrainedIntraPred != 0);
        writer.WriteBit(parameterSet.redundantPicCntPresentFlag != 0);
        writer.WriteBit(parameterSet.transform8x8ModeFlag != 0);

        writer.WriteBit(true); // pic_scaling_matrix_present_flag
        for (i32 index{}; index < 6; index++) {
            writer.WriteBit(true);
            writer.WriteScalingList(span<const u8>(context.weightScale4x4), index * 16, 16);
        }

        if (parameterSet.transform8x8ModeFlag) {
            for (i32 index{}; index < 2; index++) {
                writer.WriteBit(true);
                writer.WriteScalingList(span<const u8>(context.weightScale8x8), index * 64, 64);
            }
        }

        writer.WriteSe(static_cast<i32>(parameterSet.secondChromaQpIndexOffset));
        writer.End();

        const auto &header{writer.GetByteArray()};
        frameScratch.resize(header.size() + context.streamLength);
        std::memcpy(frameScratch.data(), header.data(), header.size());
        state.soc->smmu.Read(span<u8>(frameScratch.data() + header.size(), context.streamLength), static_cast<u32>(registers.frameBitstreamOffset.Address()));

        return frameScratch;
    }

    constexpr static i32 BufferSize{8}; //!< The size of the bit writer's accumulation buffer in bits

    void H264BitWriter::WriteU(i32 value, i32 valueSize) {
        WriteBits(value, valueSize);
    }

    void H264BitWriter::WriteSe(i32 value) {
        WriteExpGolombCodedInt(value);
    }

    void H264BitWriter::WriteUe(u32 value) {
        WriteExpGolombCodedUInt(value);
    }

    void H264BitWriter::End() {
        WriteBit(true); // RBSP stop bit
        Flush();
    }

    void H264BitWriter::WriteBit(bool state) {
        WriteBits(state ? 1 : 0, 1);
    }

    void H264BitWriter::WriteScalingList(span<const u8> list, i32 start, i32 count) {
        if (count == 16) {
            // The 4x4 zig-zag scan packed as 4 bits per index
            constexpr u64 ZigZagScan4x4{0xFEB7ADC963258410};

            u8 lastScale{8};
            for (i32 index{}; index < count; index++) {
                u8 value{list[static_cast<size_t>(start) + ((ZigZagScan4x4 >> (index * 4)) & 0xF)]};
                WriteSe(static_cast<i32>(value - lastScale));
                lastScale = value;
            }
        } else {
            constexpr std::array<u8, 64> ZigZagScan8x8{
                0, 1, 8, 16, 9, 2, 3, 10,
                17, 24, 32, 25, 18, 11, 4,
                5, 12, 19, 26, 33, 40, 48,
                41, 34, 27, 20, 13, 6, 7,
                14, 21, 28, 35, 42, 49, 56,
                57, 50, 43, 36, 29, 22, 15,
                23, 30, 37, 44, 51, 58, 59,
                52, 45, 38, 31, 39, 46, 53,
                60, 61, 54, 47, 55, 62, 63,
            };

            u8 lastScale{8};
            for (i32 index{}; index < count; index++) {
                u8 value{list[static_cast<size_t>(start) + ZigZagScan8x8[static_cast<size_t>(index)]]};
                WriteSe(static_cast<i32>(value - lastScale));
                lastScale = value;
            }
        }
    }

    void H264BitWriter::WriteBits(i32 value, i32 bitCount) {
        i32 valuePosition{};
        i32 remaining{bitCount};

        while (remaining > 0) {
            i32 copySize{std::min(remaining, GetFreeBufferBits())};

            i32 mask{(1 << copySize) - 1};
            i32 srcShift{(bitCount - valuePosition) - copySize};
            i32 dstShift{(BufferSize - bufferPosition) - copySize};

            buffer |= ((value >> srcShift) & mask) << dstShift;

            valuePosition += copySize;
            bufferPosition += copySize;
            remaining -= copySize;
        }
    }

    void H264BitWriter::WriteExpGolombCodedInt(i32 value) {
        i32 sign{value <= 0 ? 0 : 1};
        if (!sign)
            value = -value;
        WriteExpGolombCodedUInt(static_cast<u32>((value << 1) - sign));
    }

    void H264BitWriter::WriteExpGolombCodedUInt(u32 value) {
        i32 size{32 - std::countl_zero(value + 1)};
        WriteBits(1, size);

        value -= (1U << (size - 1)) - 1;
        WriteBits(static_cast<i32>(value), size - 1);
    }

    i32 H264BitWriter::GetFreeBufferBits() {
        if (bufferPosition == BufferSize)
            Flush();

        return BufferSize - bufferPosition;
    }

    void H264BitWriter::Flush() {
        if (bufferPosition == 0)
            return;

        byteArray.push_back(static_cast<u8>(buffer));
        buffer = 0;
        bufferPosition = 0;
    }
}
