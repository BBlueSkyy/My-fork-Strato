// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include <algorithm>
#include <bit>
#include <soc.h>
#include "vp9.h"

namespace skyline::soc::host1x::nvdec {
    namespace {
        constexpr u32 DiffUpdateProbability{252}; //!< The probability with which the compressed header signals that a probability update follows
        constexpr u32 FrameSyncCode{0x498342}; //!< The synchronisation code marking the start of key and intra-only frame headers

        /**
         * @brief The default compressed header probabilities used once a frame context is reset
         * @url https://www.webmproject.org/vp9/ (VP9 Bitstream & Decoding Process Specification, clause 10.5 default probability tables)
         */
        constexpr Vp9EntropyProbs DefaultProbs{
            .yModeProb{
                65, 32, 18, 144, 162, 194, 41, 51, 98, 132, 68, 18, 165, 217, 196, 45, 40, 78,
                173, 80, 19, 176, 240, 193, 64, 35, 46, 221, 135, 38, 194, 248, 121, 96, 85, 29,
            },
            .partitionProb{
                199, 122, 141, 0, 147, 63, 159, 0, 148, 133, 118, 0, 121, 104, 114, 0,
                174, 73, 87, 0, 92, 41, 83, 0, 82, 99, 50, 0, 53, 39, 39, 0,
                177, 58, 59, 0, 68, 26, 63, 0, 52, 79, 25, 0, 17, 14, 12, 0,
                222, 34, 30, 0, 72, 16, 44, 0, 58, 32, 12, 0, 10, 7, 6, 0,
            },
            .coefProbs{
                195, 29, 183, 84, 49, 136, 8, 42, 71, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                31, 107, 169, 35, 99, 159, 17, 82, 140, 8, 66, 114, 2, 44, 76, 1, 19, 32,
                40, 132, 201, 29, 114, 187, 13, 91, 157, 7, 75, 127, 3, 58, 95, 1, 28, 47,
                69, 142, 221, 42, 122, 201, 15, 91, 159, 6, 67, 121, 1, 42, 77, 1, 17, 31,
                102, 148, 228, 67, 117, 204, 17, 82, 154, 6, 59, 114, 2, 39, 75, 1, 15, 29,
                156, 57, 233, 119, 57, 212, 58, 48, 163, 29, 40, 124, 12, 30, 81, 3, 12, 31,
                191, 107, 226, 124, 117, 204, 25, 99, 155, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                29, 148, 210, 37, 126, 194, 8, 93, 157, 2, 68, 118, 1, 39, 69, 1, 17, 33,
                41, 151, 213, 27, 123, 193, 3, 82, 144, 1, 58, 105, 1, 32, 60, 1, 13, 26,
                59, 159, 220, 23, 126, 198, 4, 88, 151, 1, 66, 114, 1, 38, 71, 1, 18, 34,
                114, 136, 232, 51, 114, 207, 11, 83, 155, 3, 56, 105, 1, 33, 65, 1, 17, 34,
                149, 65, 234, 121, 57, 215, 61, 49, 166, 28, 36, 114, 12, 25, 76, 3, 16, 42,
                214, 49, 220, 132, 63, 188, 42, 65, 137, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                85, 137, 221, 104, 131, 216, 49, 111, 192, 21, 87, 155, 2, 49, 87, 1, 16, 28,
                89, 163, 230, 90, 137, 220, 29, 100, 183, 10, 70, 135, 2, 42, 81, 1, 17, 33,
                108, 167, 237, 55, 133, 222, 15, 97, 179, 4, 72, 135, 1, 45, 85, 1, 19, 38,
                124, 146, 240, 66, 124, 224, 17, 88, 175, 4, 58, 122, 1, 36, 75, 1, 18, 37,
                141, 79, 241, 126, 70, 227, 66, 58, 182, 30, 44, 136, 12, 34, 96, 2, 20, 47,
                229, 99, 249, 143, 111, 235, 46, 109, 192, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                82, 158, 236, 94, 146, 224, 25, 117, 191, 9, 87, 149, 3, 56, 99, 1, 33, 57,
                83, 167, 237, 68, 145, 222, 10, 103, 177, 2, 72, 131, 1, 41, 79, 1, 20, 39,
                99, 167, 239, 47, 141, 224, 10, 104, 178, 2, 73, 133, 1, 44, 85, 1, 22, 47,
                127, 145, 243, 71, 129, 228, 17, 93, 177, 3, 61, 124, 1, 41, 84, 1, 21, 52,
                157, 78, 244, 140, 72, 231, 69, 58, 184, 31, 44, 137, 14, 38, 105, 8, 23, 61,
                125, 34, 187, 52, 41, 133, 6, 31, 56, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                37, 109, 153, 51, 102, 147, 23, 87, 128, 8, 67, 101, 1, 41, 63, 1, 19, 29,
                31, 154, 185, 17, 127, 175, 6, 96, 145, 2, 73, 114, 1, 51, 82, 1, 28, 45,
                23, 163, 200, 10, 131, 185, 2, 93, 148, 1, 67, 111, 1, 41, 69, 1, 14, 24,
                29, 176, 217, 12, 145, 201, 3, 101, 156, 1, 69, 111, 1, 39, 63, 1, 14, 23,
                57, 192, 233, 25, 154, 215, 6, 109, 167, 3, 78, 118, 1, 48, 69, 1, 21, 29,
                202, 105, 245, 108, 106, 216, 18, 90, 144, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                33, 172, 219, 64, 149, 206, 14, 117, 177, 5, 90, 141, 2, 61, 95, 1, 37, 57,
                33, 179, 220, 11, 140, 198, 1, 89, 148, 1, 60, 104, 1, 33, 57, 1, 12, 21,
                30, 181, 221, 8, 141, 198, 1, 87, 145, 1, 58, 100, 1, 31, 55, 1, 12, 20,
                32, 186, 224, 7, 142, 198, 1, 86, 143, 1, 58, 100, 1, 31, 55, 1, 12, 22,
                57, 192, 227, 20, 143, 204, 3, 96, 154, 1, 68, 112, 1, 42, 69, 1, 19, 32,
                212, 35, 215, 113, 47, 169, 29, 48, 105, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                74, 129, 203, 106, 120, 203, 49, 107, 178, 19, 84, 144, 4, 50, 84, 1, 15, 25,
                71, 172, 217, 44, 141, 209, 15, 102, 173, 6, 76, 133, 2, 51, 89, 1, 24, 42,
                64, 185, 231, 31, 148, 216, 8, 103, 175, 3, 74, 131, 1, 46, 81, 1, 18, 30,
                65, 196, 235, 25, 157, 221, 5, 105, 174, 1, 67, 120, 1, 38, 69, 1, 15, 30,
                65, 204, 238, 30, 156, 224, 7, 107, 177, 2, 70, 124, 1, 42, 73, 1, 18, 34,
                225, 86, 251, 144, 104, 235, 42, 99, 181, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                85, 175, 239, 112, 165, 229, 29, 136, 200, 12, 103, 162, 6, 77, 123, 2, 53, 84,
                75, 183, 239, 30, 155, 221, 3, 106, 171, 1, 74, 128, 1, 44, 76, 1, 17, 28,
                73, 185, 240, 27, 159, 222, 2, 107, 172, 1, 75, 127, 1, 42, 73, 1, 17, 29,
                62, 190, 238, 21, 159, 222, 2, 107, 172, 1, 72, 122, 1, 40, 71, 1, 18, 32,
                61, 199, 240, 27, 161, 226, 4, 113, 180, 1, 76, 129, 1, 46, 80, 1, 23, 41,
                7, 27, 153, 5, 30, 95, 1, 16, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                50, 75, 127, 57, 75, 124, 27, 67, 108, 10, 54, 86, 1, 33, 52, 1, 12, 18,
                43, 125, 151, 26, 108, 148, 7, 83, 122, 2, 59, 89, 1, 38, 60, 1, 17, 27,
                23, 144, 163, 13, 112, 154, 2, 75, 117, 1, 50, 81, 1, 31, 51, 1, 14, 23,
                18, 162, 185, 6, 123, 171, 1, 78, 125, 1, 51, 86, 1, 31, 54, 1, 14, 23,
                15, 199, 227, 3, 150, 204, 1, 91, 146, 1, 55, 95, 1, 30, 53, 1, 11, 20,
                19, 55, 240, 19, 59, 196, 3, 52, 105, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                41, 166, 207, 104, 153, 199, 31, 123, 181, 14, 101, 152, 5, 72, 106, 1, 36, 52,
                35, 176, 211, 12, 131, 190, 2, 88, 144, 1, 60, 101, 1, 36, 60, 1, 16, 28,
                28, 183, 213, 8, 134, 191, 1, 86, 142, 1, 56, 96, 1, 30, 53, 1, 12, 20,
                20, 190, 215, 4, 135, 192, 1, 84, 139, 1, 53, 91, 1, 28, 49, 1, 11, 20,
                13, 196, 216, 2, 137, 192, 1, 86, 143, 1, 57, 99, 1, 32, 56, 1, 13, 24,
                211, 29, 217, 96, 47, 156, 22, 43, 87, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                78, 120, 193, 111, 116, 186, 46, 102, 164, 15, 80, 128, 2, 49, 76, 1, 18, 28,
                71, 161, 203, 42, 132, 192, 10, 98, 150, 3, 69, 109, 1, 44, 70, 1, 18, 29,
                57, 186, 211, 30, 140, 196, 4, 93, 146, 1, 62, 102, 1, 38, 65, 1, 16, 27,
                47, 199, 217, 14, 145, 196, 1, 88, 142, 1, 57, 98, 1, 36, 62, 1, 15, 26,
                26, 219, 229, 5, 155, 207, 1, 94, 151, 1, 60, 104, 1, 36, 62, 1, 16, 28,
                233, 29, 248, 146, 47, 220, 43, 52, 140, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                100, 163, 232, 179, 161, 222, 63, 142, 204, 37, 113, 174, 26, 89, 137, 18, 68, 97,
                85, 181, 230, 32, 146, 209, 7, 100, 164, 3, 71, 121, 1, 45, 77, 1, 18, 30,
                65, 187, 230, 20, 148, 207, 2, 97, 159, 1, 68, 116, 1, 40, 70, 1, 14, 29,
                40, 194, 227, 8, 147, 204, 1, 94, 155, 1, 65, 112, 1, 39, 66, 1, 14, 26,
                16, 208, 228, 3, 151, 207, 1, 98, 160, 1, 67, 117, 1, 41, 74, 1, 17, 31,
                17, 38, 140, 7, 34, 80, 1, 17, 29, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                37, 75, 128, 41, 76, 128, 26, 66, 116, 12, 52, 94, 2, 32, 55, 1, 10, 16,
                50, 127, 154, 37, 109, 152, 16, 82, 121, 5, 59, 85, 1, 35, 54, 1, 13, 20,
                40, 142, 167, 17, 110, 157, 2, 71, 112, 1, 44, 72, 1, 27, 45, 1, 11, 17,
                30, 175, 188, 9, 124, 169, 1, 74, 116, 1, 48, 78, 1, 30, 49, 1, 11, 18,
                10, 222, 223, 2, 150, 194, 1, 83, 128, 1, 48, 79, 1, 27, 45, 1, 11, 17,
                36, 41, 235, 29, 36, 193, 10, 27, 111, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                85, 165, 222, 177, 162, 215, 110, 135, 195, 57, 113, 168, 23, 83, 120, 10, 49, 61,
                85, 190, 223, 36, 139, 200, 5, 90, 146, 1, 60, 103, 1, 38, 65, 1, 18, 30,
                72, 202, 223, 23, 141, 199, 2, 86, 140, 1, 56, 97, 1, 36, 61, 1, 16, 27,
                55, 218, 225, 13, 145, 200, 1, 86, 141, 1, 57, 99, 1, 35, 61, 1, 13, 22,
                15, 235, 212, 1, 132, 184, 1, 84, 139, 1, 57, 97, 1, 34, 56, 1, 14, 23,
                181, 21, 201, 61, 37, 123, 10, 38, 71, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                47, 106, 172, 95, 104, 173, 42, 93, 159, 18, 77, 131, 4, 50, 81, 1, 17, 23,
                62, 147, 199, 44, 130, 189, 28, 102, 154, 18, 75, 115, 2, 44, 65, 1, 12, 19,
                55, 153, 210, 24, 130, 194, 3, 93, 146, 1, 61, 97, 1, 31, 50, 1, 10, 16,
                49, 186, 223, 17, 148, 204, 1, 96, 142, 1, 53, 83, 1, 26, 44, 1, 11, 17,
                13, 217, 212, 2, 136, 180, 1, 78, 124, 1, 50, 83, 1, 29, 49, 1, 14, 23,
                197, 13, 247, 82, 17, 222, 25, 17, 162, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                126, 186, 247, 234, 191, 243, 176, 177, 234, 104, 158, 220, 66, 128, 186, 55, 90, 137,
                111, 197, 242, 46, 158, 219, 9, 104, 171, 2, 65, 125, 1, 44, 80, 1, 17, 91,
                104, 208, 245, 39, 168, 224, 3, 109, 162, 1, 79, 124, 1, 50, 102, 1, 43, 102,
                84, 220, 246, 31, 177, 231, 2, 115, 180, 1, 79, 134, 1, 55, 77, 1, 60, 79,
                43, 243, 240, 8, 180, 217, 1, 115, 166, 1, 84, 121, 1, 51, 67, 1, 16, 6,
            },
            .switchableInterpProb{235, 162, 36, 255, 34, 3, 149, 144},
            .interModeProb{
                2, 173, 34, 0, 7, 145, 85, 0, 7, 166, 63, 0, 7, 94,
                66, 0, 8, 64, 46, 0, 17, 81, 31, 0, 25, 29, 30, 0,
            },
            .intraInterProb{9, 102, 187, 225},
            .compInterProb{9, 102, 187, 225, 0},
            .singleRefProb{33, 16, 77, 74, 142, 142, 172, 170, 238, 247},
            .compRefProb{50, 126, 123, 221, 226},
            .tx32x32Prob{3, 136, 37, 5, 52, 13},
            .tx16x16Prob{20, 152, 15, 101},
            .tx8x8Prob{100, 66},
            .skipProbs{192, 128, 64},
            .joints{32, 64, 96},
            .sign{128, 128},
            .classes{
                224, 144, 192, 168, 192, 176, 192, 198, 198, 245,
                216, 128, 176, 160, 176, 176, 192, 198, 198, 208,
            },
            .class0{216, 208},
            .probBits{
                136, 140, 148, 160, 176, 192, 224, 234, 234, 240,
                136, 140, 148, 160, 176, 192, 224, 234, 234, 240,
            },
            .class0Fr{128, 128, 64, 96, 112, 64, 128, 128, 64, 96, 112, 64},
            .fr{64, 96, 64, 64, 96, 64},
            .class0Hp{160, 160},
            .highPrecision{128, 128},
        };

        /**
         * @brief Computes the minimum log2 number of tile columns per clause 6.2.14 (tile size calculation) of the VP9 specification
         */
        i32 CalcMinLog2TileCols(i32 frameWidth) {
            const i32 sb64Cols{(frameWidth + 63) / 64};
            i32 minLog2{};

            while ((64 << minLog2) < sb64Cols)
                minLog2++;

            return minLog2;
        }

        /**
         * @brief Computes the maximum log2 number of tile columns per clause 6.2.14 (tile size calculation) of the VP9 specification
         */
        i32 CalcMaxLog2TileCols(i32 frameWidth) {
            const i32 sb64Cols{(frameWidth + 63) / 64};
            i32 maxLog2{1};

            while ((sb64Cols >> maxLog2) >= 4)
                maxLog2++;

            return maxLog2 - 1;
        }

        /**
         * @brief Recenters a probability, the inverse of clause 6.3.6 (inv_recenter_nonneg) of the VP9 specification
         */
        i32 RecenterNonNeg(i32 newProb, i32 oldProb) {
            if (newProb > oldProb * 2)
                return newProb;

            if (newProb >= oldProb)
                return (newProb - oldProb) * 2;

            return (oldProb - newProb) * 2 - 1;
        }

        /**
         * @brief Adjusts oldProb depending on newProb, the inverse of clause 6.3.5 (inv_remap_prob) of the VP9 specification
         */
        i32 RemapProbability(i32 newProb, i32 oldProb) {
            newProb--;
            oldProb--;

            u8 index{oldProb * 2 <= 0xFF ? static_cast<u8>(std::max(0, RecenterNonNeg(newProb, oldProb) - 1))
                                         : static_cast<u8>(std::max(0, RecenterNonNeg(0xFF - 1 - newProb, 0xFF - 1 - oldProb) - 1))};

            return (index + 7) % 13 == 0 ? (index + 7) / 13 - 1 : index + 20 - (index + 7) / 13;
        }
    }

    Vp9::Vp9(const DeviceState &state, const Registers &registers) : Codec(state, registers) {
        initialized = decoder.Initialize(CodecId::Vp9);
    }

    u64 Vp9::GetOutputLumaAddress() {
        return registers.surfaceLumaOffsets[static_cast<size_t>(Vp9SurfaceIndex::Current)].Address();
    }

    void Vp9::WriteProbabilityUpdate(VpxRangeEncoder &writer, u8 newProb, u8 oldProb) {
        const bool update{newProb != oldProb};

        writer.Write(update, DiffUpdateProbability);

        if (update)
            WriteProbabilityDelta(writer, newProb, oldProb);
    }

    template<typename T, size_t N>
    void Vp9::WriteProbabilityUpdate(VpxRangeEncoder &writer, const std::array<T, N> &newProb, const std::array<T, N> &oldProb) {
        for (size_t offset{}; offset < newProb.size(); offset++)
            WriteProbabilityUpdate(writer, newProb[offset], oldProb[offset]);
    }

    template<typename T, size_t N>
    void Vp9::WriteProbabilityUpdateAligned4(VpxRangeEncoder &writer, const std::array<T, N> &newProb, const std::array<T, N> &oldProb) {
        for (size_t offset{}; offset < newProb.size(); offset += 4) {
            WriteProbabilityUpdate(writer, newProb[offset + 0], oldProb[offset + 0]);
            WriteProbabilityUpdate(writer, newProb[offset + 1], oldProb[offset + 1]);
            WriteProbabilityUpdate(writer, newProb[offset + 2], oldProb[offset + 2]);
        }
    }

    void Vp9::WriteProbabilityDelta(VpxRangeEncoder &writer, u8 newProb, u8 oldProb) {
        const i32 delta{RemapProbability(newProb, oldProb)};

        EncodeTermSubExp(writer, delta);
    }

    void Vp9::EncodeTermSubExp(VpxRangeEncoder &writer, i32 value) {
        if (WriteLessThan(writer, value, 16)) {
            writer.Write(value, 4);
        } else if (WriteLessThan(writer, value, 32)) {
            writer.Write(value - 16, 4);
        } else if (WriteLessThan(writer, value, 64)) {
            writer.Write(value - 32, 5);
        } else {
            value -= 64;

            constexpr i32 Size{8};

            const i32 mask{(1 << Size) - 191};
            const i32 delta{value - mask};

            if (delta < 0) {
                writer.Write(value, Size - 1);
            } else {
                writer.Write(delta / 2 + mask, Size - 1);
                writer.Write(delta & 1, 1);
            }
        }
    }

    bool Vp9::WriteLessThan(VpxRangeEncoder &writer, i32 value, i32 test) {
        const bool isLessThan{value < test};
        writer.Write(!isLessThan);
        return isLessThan;
    }

    void Vp9::WriteCoefProbabilityUpdate(VpxRangeEncoder &writer, i32 txMode, const std::array<u8, 1728> &newProb, const std::array<u8, 1728> &oldProb) {
        constexpr u32 BlockBytes{2 * 2 * 6 * 6 * 3};

        const auto needsUpdate{[&](u32 baseIndex) {
            return !std::equal(newProb.begin() + baseIndex, newProb.begin() + baseIndex + BlockBytes, oldProb.begin() + baseIndex);
        }};

        for (u32 blockIndex{}; blockIndex < 4; blockIndex++) {
            const u32 baseIndex{blockIndex * BlockBytes};
            const bool update{needsUpdate(baseIndex)};
            writer.Write(update);

            if (update) {
                u32 index{baseIndex};
                for (i32 i{}; i < 2; i++) {
                    for (i32 j{}; j < 2; j++) {
                        for (i32 k{}; k < 6; k++) {
                            for (i32 l{}; l < 6; l++) {
                                if (k != 0 || l < 3) {
                                    WriteProbabilityUpdate(writer, newProb[index + 0], oldProb[index + 0]);
                                    WriteProbabilityUpdate(writer, newProb[index + 1], oldProb[index + 1]);
                                    WriteProbabilityUpdate(writer, newProb[index + 2], oldProb[index + 2]);
                                }
                                index += 3;
                            }
                        }
                    }
                }
            }

            if (blockIndex == static_cast<u32>(txMode))
                break;
        }
    }

    void Vp9::WriteMvProbabilityUpdate(VpxRangeEncoder &writer, u8 newProb, u8 oldProb) {
        const bool update{newProb != oldProb};
        writer.Write(update, DiffUpdateProbability);

        if (update)
            writer.Write(newProb >> 1, 7);
    }

    void Vp9::WriteSegmentation(VpxBitStreamWriter &writer) {
        bool enabled{currentPictureInfo.segmentation.enabled != 0};
        writer.WriteBit(enabled);
        if (!enabled)
            return;

        bool updateMap{currentPictureInfo.segmentation.updateMap != 0};
        writer.WriteBit(updateMap);

        if (updateMap) {
            auto entropyProbs{state.soc->smmu.Read<Vp9GuestEntropyProbs>(static_cast<u32>(registers.vp9ProbTabBufferOffset.Address()))};

            auto writeProb{[&](u8 prob) {
                bool coded{prob != 255};
                writer.WriteBit(coded);
                if (coded)
                    writer.WriteU(prob, 8);
            }};

            for (const auto &prob : entropyProbs.mbSegmentTreeProbs)
                writeProb(prob);

            bool temporalUpdate{currentPictureInfo.segmentation.temporalUpdate != 0};
            writer.WriteBit(temporalUpdate);

            if (temporalUpdate) {
                for (const auto &prob : entropyProbs.segmentPredProbs)
                    writeProb(prob);
            }
        }

        if (lastSegmentation == currentPictureInfo.segmentation) {
            writer.WriteBit(false);
            return;
        }

        lastSegmentation = currentPictureInfo.segmentation;
        writer.WriteBit(true);
        writer.WriteBit(currentPictureInfo.segmentation.absDelta != 0);

        constexpr size_t MaxSegments{8};
        constexpr std::array<u32, 4> SegmentationFeatureBits{8, 6, 2, 0};

        for (size_t i{}; i < MaxSegments; i++) {
            bool qEnabled{currentPictureInfo.segmentation.featureEnabled[i][0] != 0};
            writer.WriteBit(qEnabled);
            if (qEnabled)
                writer.WriteS(currentPictureInfo.segmentation.featureData[i][0], SegmentationFeatureBits[0]);

            bool lfEnabled{currentPictureInfo.segmentation.featureEnabled[i][1] != 0};
            writer.WriteBit(lfEnabled);
            if (lfEnabled)
                writer.WriteS(currentPictureInfo.segmentation.featureData[i][1], SegmentationFeatureBits[1]);

            bool refEnabled{currentPictureInfo.segmentation.featureEnabled[i][2] != 0};
            writer.WriteBit(refEnabled);
            if (refEnabled)
                writer.WriteU(static_cast<u32>(currentPictureInfo.segmentation.featureData[i][2]), SegmentationFeatureBits[2]);

            bool skipEnabled{currentPictureInfo.segmentation.featureEnabled[i][3] != 0};
            writer.WriteBit(skipEnabled);
        }
    }

    Vp9PictureInfo Vp9::GetVp9PictureInfo() {
        currentPictureInfo = state.soc->smmu.Read<Vp9GuestPictureInfo>(static_cast<u32>(registers.pictureInfoOffset.Address()));
        auto info{currentPictureInfo.Convert()};

        InsertEntropy(registers.vp9ProbTabBufferOffset.Address(), info.entropy);

        // surfaceLumaOffsets[0:3] hold the reference frame addresses in the order: last, golden, altref, current
        for (size_t i{}; i < info.frameOffsets.size(); i++)
            info.frameOffsets[i] = registers.surfaceLumaOffsets[i].Address();

        return info;
    }

    void Vp9::InsertEntropy(u64 offset, Vp9EntropyProbs &dst) {
        auto entropy{state.soc->smmu.Read<Vp9GuestEntropyProbs>(static_cast<u32>(offset))};
        entropy.Convert(dst);
    }

    Vp9FrameContainer Vp9::GetCurrentFrame() {
        Vp9FrameContainer currentFrame{};
        currentFrame.info = GetVp9PictureInfo();

        if (!registers.frameBitstreamOffset.raw || !currentFrame.info.bitstreamSize || currentFrame.info.bitstreamSize > MaxBitstreamSize) {
            LOGW("Invalid VP9 bitstream, offset: 0x{:X}, size: 0x{:X}", registers.frameBitstreamOffset.Address(), currentFrame.info.bitstreamSize);
            return {};
        }

        currentFrame.bitstream.resize(currentFrame.info.bitstreamSize);
        state.soc->smmu.Read(span<u8>(currentFrame.bitstream), static_cast<u32>(registers.frameBitstreamOffset.Address()));

        if (!nextFrame.bitstream.empty()) {
            Vp9FrameContainer temp{
                .info = currentFrame.info,
                .bitstream = std::move(currentFrame.bitstream),
            };
            nextFrame.info.showFrame = currentFrame.info.lastFrameShown;
            currentFrame.info = nextFrame.info;
            currentFrame.bitstream = std::move(nextFrame.bitstream);
            nextFrame = std::move(temp);
        } else {
            nextFrame.info = currentFrame.info;
            nextFrame.bitstream = currentFrame.bitstream;
        }

        return currentFrame;
    }

    std::vector<u8> Vp9::ComposeCompressedHeader() {
        VpxRangeEncoder writer;
        const bool updateProbs{!currentFrameInfo.isKeyFrame && currentFrameInfo.showFrame};

        if (!currentFrameInfo.lossless) {
            if (static_cast<u32>(currentFrameInfo.transformMode) >= 3) {
                writer.Write(3, 2);
                writer.Write(currentFrameInfo.transformMode == 4);
            } else {
                writer.Write(currentFrameInfo.transformMode, 2);
            }
        }

        if (currentFrameInfo.transformMode == 4) {
            // tx_mode_probs() in the spec
            WriteProbabilityUpdate(writer, currentFrameInfo.entropy.tx8x8Prob, prevFrameProbs.tx8x8Prob);
            WriteProbabilityUpdate(writer, currentFrameInfo.entropy.tx16x16Prob, prevFrameProbs.tx16x16Prob);
            WriteProbabilityUpdate(writer, currentFrameInfo.entropy.tx32x32Prob, prevFrameProbs.tx32x32Prob);
            if (updateProbs) {
                prevFrameProbs.tx8x8Prob = currentFrameInfo.entropy.tx8x8Prob;
                prevFrameProbs.tx16x16Prob = currentFrameInfo.entropy.tx16x16Prob;
                prevFrameProbs.tx32x32Prob = currentFrameInfo.entropy.tx32x32Prob;
            }
        }

        // read_coef_probs() in the spec
        WriteCoefProbabilityUpdate(writer, currentFrameInfo.transformMode, currentFrameInfo.entropy.coefProbs, prevFrameProbs.coefProbs);
        // read_skip_probs() in the spec
        WriteProbabilityUpdate(writer, currentFrameInfo.entropy.skipProbs, prevFrameProbs.skipProbs);

        if (updateProbs) {
            prevFrameProbs.coefProbs = currentFrameInfo.entropy.coefProbs;
            prevFrameProbs.skipProbs = currentFrameInfo.entropy.skipProbs;
        }

        if (!currentFrameInfo.intraOnly) {
            // read_inter_probs() in the spec
            WriteProbabilityUpdateAligned4(writer, currentFrameInfo.entropy.interModeProb, prevFrameProbs.interModeProb);

            if (currentFrameInfo.interpFilter == 4) {
                // read_interp_filter_probs() in the spec
                WriteProbabilityUpdate(writer, currentFrameInfo.entropy.switchableInterpProb, prevFrameProbs.switchableInterpProb);
                if (updateProbs)
                    prevFrameProbs.switchableInterpProb = currentFrameInfo.entropy.switchableInterpProb;
            }

            // read_is_inter_probs() in the spec
            WriteProbabilityUpdate(writer, currentFrameInfo.entropy.intraInterProb, prevFrameProbs.intraInterProb);

            // frame_reference_mode() in the spec
            if ((currentFrameInfo.refFrameSignBias[1] & 1) != (currentFrameInfo.refFrameSignBias[2] & 1) ||
                (currentFrameInfo.refFrameSignBias[1] & 1) != (currentFrameInfo.refFrameSignBias[3] & 1)) {
                if (currentFrameInfo.referenceMode >= 1) {
                    writer.Write(1, 1);
                    writer.Write(currentFrameInfo.referenceMode == 2);
                } else {
                    writer.Write(0, 1);
                }
            }

            // frame_reference_mode_probs() in the spec
            if (currentFrameInfo.referenceMode == 2) {
                WriteProbabilityUpdate(writer, currentFrameInfo.entropy.compInterProb, prevFrameProbs.compInterProb);
                if (updateProbs)
                    prevFrameProbs.compInterProb = currentFrameInfo.entropy.compInterProb;
            }

            if (currentFrameInfo.referenceMode != 1) {
                WriteProbabilityUpdate(writer, currentFrameInfo.entropy.singleRefProb, prevFrameProbs.singleRefProb);
                if (updateProbs)
                    prevFrameProbs.singleRefProb = currentFrameInfo.entropy.singleRefProb;
            }

            if (currentFrameInfo.referenceMode != 0) {
                WriteProbabilityUpdate(writer, currentFrameInfo.entropy.compRefProb, prevFrameProbs.compRefProb);
                if (updateProbs)
                    prevFrameProbs.compRefProb = currentFrameInfo.entropy.compRefProb;
            }

            // read_y_mode_probs() in the spec
            for (size_t index{}; index < currentFrameInfo.entropy.yModeProb.size(); index++)
                WriteProbabilityUpdate(writer, currentFrameInfo.entropy.yModeProb[index], prevFrameProbs.yModeProb[index]);

            // read_partition_probs() in the spec
            WriteProbabilityUpdateAligned4(writer, currentFrameInfo.entropy.partitionProb, prevFrameProbs.partitionProb);

            // mv_probs() in the spec
            for (size_t i{}; i < 3; i++)
                WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.joints[i], prevFrameProbs.joints[i]);

            if (updateProbs) {
                prevFrameProbs.interModeProb = currentFrameInfo.entropy.interModeProb;
                prevFrameProbs.intraInterProb = currentFrameInfo.entropy.intraInterProb;
                prevFrameProbs.yModeProb = currentFrameInfo.entropy.yModeProb;
                prevFrameProbs.partitionProb = currentFrameInfo.entropy.partitionProb;
                prevFrameProbs.joints = currentFrameInfo.entropy.joints;
            }

            for (size_t i{}; i < 2; i++) {
                WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.sign[i], prevFrameProbs.sign[i]);

                for (size_t j{}; j < 10; j++) {
                    const size_t index{i * 10 + j};
                    WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.classes[index], prevFrameProbs.classes[index]);
                }

                WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.class0[i], prevFrameProbs.class0[i]);

                for (size_t j{}; j < 10; j++) {
                    const size_t index{i * 10 + j};
                    WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.probBits[index], prevFrameProbs.probBits[index]);
                }
            }

            for (size_t i{}; i < 2; i++) {
                for (size_t j{}; j < 2; j++) {
                    for (size_t k{}; k < 3; k++) {
                        const size_t index{i * 2 * 3 + j * 3 + k};
                        WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.class0Fr[index], prevFrameProbs.class0Fr[index]);
                    }
                }

                for (size_t j{}; j < 3; j++) {
                    const size_t index{i * 3 + j};
                    WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.fr[index], prevFrameProbs.fr[index]);
                }
            }

            if (currentFrameInfo.allowHighPrecisionMv) {
                for (size_t index{}; index < 2; index++) {
                    WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.class0Hp[index], prevFrameProbs.class0Hp[index]);
                    WriteMvProbabilityUpdate(writer, currentFrameInfo.entropy.highPrecision[index], prevFrameProbs.highPrecision[index]);
                }
            }

            // Save the previous motion vector probabilities
            if (updateProbs) {
                prevFrameProbs.sign = currentFrameInfo.entropy.sign;
                prevFrameProbs.classes = currentFrameInfo.entropy.classes;
                prevFrameProbs.class0 = currentFrameInfo.entropy.class0;
                prevFrameProbs.probBits = currentFrameInfo.entropy.probBits;
                prevFrameProbs.class0Fr = currentFrameInfo.entropy.class0Fr;
                prevFrameProbs.fr = currentFrameInfo.entropy.fr;
                prevFrameProbs.class0Hp = currentFrameInfo.entropy.class0Hp;
                prevFrameProbs.highPrecision = currentFrameInfo.entropy.highPrecision;
            }
        }

        writer.End();
        return writer.GetBuffer();
    }

    VpxBitStreamWriter Vp9::ComposeUncompressedHeader() {
        VpxBitStreamWriter writer;

        writer.WriteU(2, 2); // Frame marker
        writer.WriteU(0, 2); // Profile
        writer.WriteBit(false); // Show existing frame
        writer.WriteBit(!currentFrameInfo.isKeyFrame); // Frame type
        writer.WriteBit(currentFrameInfo.showFrame); // Show frame
        writer.WriteBit(currentFrameInfo.errorResilientMode); // Error resilient mode

        if (currentFrameInfo.isKeyFrame) {
            writer.WriteU(FrameSyncCode, 24);
            writer.WriteU(0, 3); // Color space
            writer.WriteU(0, 1); // Color range
            writer.WriteU(static_cast<u32>(currentFrameInfo.frameSize.width - 1), 16);
            writer.WriteU(static_cast<u32>(currentFrameInfo.frameSize.height - 1), 16);
            writer.WriteBit(false); // Render and frame size different

            // Reset the frame contexts
            prevFrameProbs = DefaultProbs;
            swapRefIndices = false;
            loopFilterRefDeltas.fill(0);
            loopFilterModeDeltas.fill(0);
            frameContexts.fill(DefaultProbs);

            // Intra only, meaning the frame can be recreated with no other references
            currentFrameInfo.intraOnly = true;
        } else {
            if (!currentFrameInfo.showFrame)
                writer.WriteBit(currentFrameInfo.intraOnly);
            else
                currentFrameInfo.intraOnly = false;

            if (!currentFrameInfo.errorResilientMode)
                writer.WriteU(0, 2); // Reset frame context

            const auto &currOffsets{currentFrameInfo.frameOffsets};
            const auto &nextOffsets{nextFrame.info.frameOffsets};
            const bool refFramesDifferent{currOffsets[1] != currOffsets[2]};
            const bool nextReferencesSwap{(nextOffsets[1] == currOffsets[2]) || (nextOffsets[2] == currOffsets[1])};
            const bool needsRefSwap{refFramesDifferent && nextReferencesSwap};
            if (needsRefSwap)
                swapRefIndices = !swapRefIndices;

            u32 refreshFrameFlags{};
            for (u32 index{}; index < 3; index++) {
                // Refresh indices that use the current frame as an index
                if (currOffsets[3] == nextOffsets[index])
                    refreshFrameFlags |= 1U << index;
            }

            if (swapRefIndices) {
                // Swap the golden (bit 1) and altref (bit 2) refresh bits
                const u32 goldenBit{(refreshFrameFlags >> 1) & 1};
                const u32 altBit{(refreshFrameFlags >> 2) & 1};
                refreshFrameFlags = (refreshFrameFlags & 0b001) | (altBit << 1) | (goldenBit << 2);
            }

            if (currentFrameInfo.intraOnly) {
                writer.WriteU(FrameSyncCode, 24);
                writer.WriteU(refreshFrameFlags, 8);
                writer.WriteU(static_cast<u32>(currentFrameInfo.frameSize.width - 1), 16);
                writer.WriteU(static_cast<u32>(currentFrameInfo.frameSize.height - 1), 16);
                writer.WriteBit(false); // Render and frame size different
            } else {
                const bool swapIndices{needsRefSwap != swapRefIndices};
                const auto refFrameIndex{swapIndices ? std::array<u32, 3>{0, 2, 1} : std::array<u32, 3>{0, 1, 2}};
                writer.WriteU(refreshFrameFlags, 8);

                for (size_t index{1}; index < 4; index++) {
                    writer.WriteU(refFrameIndex[index - 1], 3);
                    writer.WriteU(static_cast<u32>(currentFrameInfo.refFrameSignBias[index]), 1);
                }

                writer.WriteBit(true); // Frame size with refs
                writer.WriteBit(false); // Render and frame size different
                writer.WriteBit(currentFrameInfo.allowHighPrecisionMv);
                writer.WriteBit(currentFrameInfo.interpFilter == 4);

                if (currentFrameInfo.interpFilter != 4)
                    writer.WriteU(static_cast<u32>(currentFrameInfo.interpFilter), 2);
            }
        }

        if (!currentFrameInfo.errorResilientMode) {
            writer.WriteBit(true); // Refresh frame context
            writer.WriteBit(true); // Frame parallel decoding mode
        }

        i32 frameCtxIdx{};
        if (!currentFrameInfo.showFrame)
            frameCtxIdx = 1;

        writer.WriteU(static_cast<u32>(frameCtxIdx), 2); // Frame context index
        prevFrameProbs = frameContexts[static_cast<size_t>(frameCtxIdx)]; // Reference probabilities for the compressed header
        frameContexts[static_cast<size_t>(frameCtxIdx)] = currentFrameInfo.entropy;

        writer.WriteU(currentFrameInfo.firstLevel, 6);
        writer.WriteU(currentFrameInfo.sharpnessLevel, 3);
        writer.WriteBit(currentFrameInfo.modeRefDeltaEnabled);

        if (currentFrameInfo.modeRefDeltaEnabled) {
            // Check if the loop filter deltas differ, update accordingly
            std::array<bool, 4> updateLoopFilterRefDeltas{};
            std::array<bool, 2> updateLoopFilterModeDeltas{};

            bool loopFilterDeltaUpdate{};

            for (size_t index{}; index < currentFrameInfo.refDeltas.size(); index++) {
                const i8 oldDeltas{loopFilterRefDeltas[index]};
                const i8 newDeltas{currentFrameInfo.refDeltas[index]};
                const bool differingDelta{oldDeltas != newDeltas};

                updateLoopFilterRefDeltas[index] = differingDelta;
                loopFilterDeltaUpdate = loopFilterDeltaUpdate || differingDelta;
            }

            for (size_t index{}; index < currentFrameInfo.modeDeltas.size(); index++) {
                const i8 oldDeltas{loopFilterModeDeltas[index]};
                const i8 newDeltas{currentFrameInfo.modeDeltas[index]};
                const bool differingDelta{oldDeltas != newDeltas};

                updateLoopFilterModeDeltas[index] = differingDelta;
                loopFilterDeltaUpdate = loopFilterDeltaUpdate || differingDelta;
            }

            writer.WriteBit(loopFilterDeltaUpdate);

            if (loopFilterDeltaUpdate) {
                for (size_t index{}; index < currentFrameInfo.refDeltas.size(); index++) {
                    writer.WriteBit(updateLoopFilterRefDeltas[index]);

                    if (updateLoopFilterRefDeltas[index])
                        writer.WriteS(currentFrameInfo.refDeltas[index], 6);
                }

                for (size_t index{}; index < currentFrameInfo.modeDeltas.size(); index++) {
                    writer.WriteBit(updateLoopFilterModeDeltas[index]);

                    if (updateLoopFilterModeDeltas[index])
                        writer.WriteS(currentFrameInfo.modeDeltas[index], 6);
                }

                // Save the new deltas
                loopFilterRefDeltas = currentFrameInfo.refDeltas;
                loopFilterModeDeltas = currentFrameInfo.modeDeltas;
            }
        }

        writer.WriteU(static_cast<u32>(currentFrameInfo.baseQIndex), 8);

        writer.WriteDeltaQ(static_cast<u32>(currentFrameInfo.yDcDeltaQ));
        writer.WriteDeltaQ(static_cast<u32>(currentFrameInfo.uvDcDeltaQ));
        writer.WriteDeltaQ(static_cast<u32>(currentFrameInfo.uvAcDeltaQ));

        WriteSegmentation(writer);

        const i32 minTileColsLog2{CalcMinLog2TileCols(currentFrameInfo.frameSize.width)};
        const i32 maxTileColsLog2{CalcMaxLog2TileCols(currentFrameInfo.frameSize.width)};

        const i32 tileColsLog2Diff{currentFrameInfo.log2TileCols - minTileColsLog2};
        const i32 tileColsLog2IncMask{(1 << tileColsLog2Diff) - 1};

        // If it's less than the maximum, an extra zero bit is added to indicate that reading should stop
        if (currentFrameInfo.log2TileCols < maxTileColsLog2)
            writer.WriteU(static_cast<u32>(tileColsLog2IncMask << 1), static_cast<u32>(tileColsLog2Diff + 1));
        else
            writer.WriteU(static_cast<u32>(tileColsLog2IncMask), static_cast<u32>(tileColsLog2Diff));

        const bool tileRowsLog2IsNonzero{currentFrameInfo.log2TileRows != 0};

        writer.WriteBit(tileRowsLog2IsNonzero);

        if (tileRowsLog2IsNonzero)
            writer.WriteBit(currentFrameInfo.log2TileRows > 1);

        return writer;
    }

    span<const u8> Vp9::ComposeBitstream() {
        hiddenFrame = false;

        std::vector<u8> bitstream;
        {
            auto currentFrame{GetCurrentFrame()};
            if (currentFrame.bitstream.empty())
                return {};

            currentFrameInfo = currentFrame.info;
            bitstream = std::move(currentFrame.bitstream);
        }

        // The uncompressed header routine sets the previous probabilities which the compressed header requires
        auto uncompWriter{ComposeUncompressedHeader()};
        std::vector<u8> compressedHeader{ComposeCompressedHeader()};

        uncompWriter.WriteU(static_cast<u32>(compressedHeader.size()), 16);
        uncompWriter.Flush();
        const auto &uncompressedHeader{uncompWriter.GetByteArray()};

        // Write the headers and the frame to the packet buffer
        frameScratch.resize(uncompressedHeader.size() + compressedHeader.size() + bitstream.size());
        std::memcpy(frameScratch.data(), uncompressedHeader.data(), uncompressedHeader.size());
        std::memcpy(frameScratch.data() + uncompressedHeader.size(), compressedHeader.data(), compressedHeader.size());
        std::memcpy(frameScratch.data() + uncompressedHeader.size() + compressedHeader.size(), bitstream.data(), bitstream.size());

        hiddenFrame = !currentFrameInfo.showFrame;

        return frameScratch;
    }

    VpxRangeEncoder::VpxRangeEncoder() {
        Write(false);
    }

    void VpxRangeEncoder::Write(i32 value, i32 valueSize) {
        for (i32 bit{valueSize - 1}; bit >= 0; bit--)
            Write(((value >> bit) & 1) != 0);
    }

    void VpxRangeEncoder::Write(bool bit) {
        Write(bit, HalfProbability);
    }

    void VpxRangeEncoder::Write(bool bit, i32 probability) {
        u32 localRange{range};
        const u32 split{1 + (((localRange - 1) * static_cast<u32>(probability)) >> 8)};
        localRange = split;

        if (bit) {
            lowValue += split;
            localRange = range - split;
        }

        i32 shift{localRange == 0 ? 0 : std::countl_zero(localRange) - 24};
        localRange <<= shift;
        count += shift;

        if (count >= 0) {
            const i32 offset{shift - count};

            if (((lowValue << (offset - 1)) >> 31) != 0) {
                // Propagate the carry through any 0xFF bytes preceding the write position
                size_t position{buffer.size()};
                while (position != 0 && buffer[position - 1] == 0xFF) {
                    buffer[position - 1] = 0;
                    position--;
                }

                if (position != 0)
                    buffer[position - 1]++;
            }

            buffer.push_back(static_cast<u8>(lowValue >> (24 - offset)));

            lowValue <<= offset;
            shift = count;
            lowValue &= 0xFFFFFF;
            count -= 8;
        }

        lowValue <<= shift;
        range = localRange;
    }

    void VpxRangeEncoder::End() {
        for (size_t index{}; index < 32; index++)
            Write(false);
    }

    constexpr static i32 BufferSize{8}; //!< The size of the bit writer's accumulation buffer in bits

    void VpxBitStreamWriter::WriteU(u32 value, u32 valueSize) {
        WriteBits(value, valueSize);
    }

    void VpxBitStreamWriter::WriteS(i32 value, u32 valueSize) {
        const bool sign{value < 0};
        if (sign)
            value = -value;

        WriteBits((static_cast<u32>(value) << 1) | (sign ? 1 : 0), valueSize + 1);
    }

    void VpxBitStreamWriter::WriteDeltaQ(u32 value) {
        const bool deltaCoded{value != 0};
        WriteBit(deltaCoded);

        if (deltaCoded)
            WriteBits(value, 4);
    }

    void VpxBitStreamWriter::WriteBits(u32 value, u32 bitCount) {
        i32 valuePosition{};
        i32 remaining{static_cast<i32>(bitCount)};

        while (remaining > 0) {
            i32 copySize{std::min(remaining, GetFreeBufferBits())};

            i32 mask{(1 << copySize) - 1};
            i32 srcShift{(static_cast<i32>(bitCount) - valuePosition) - copySize};
            i32 dstShift{(BufferSize - bufferPosition) - copySize};

            buffer |= (static_cast<i32>(value >> srcShift) & mask) << dstShift;

            valuePosition += copySize;
            bufferPosition += copySize;
            remaining -= copySize;
        }
    }

    void VpxBitStreamWriter::WriteBit(bool state) {
        WriteBits(state ? 1 : 0, 1);
    }

    i32 VpxBitStreamWriter::GetFreeBufferBits() {
        if (bufferPosition == BufferSize)
            Flush();

        return BufferSize - bufferPosition;
    }

    void VpxBitStreamWriter::Flush() {
        if (bufferPosition == 0)
            return;

        byteArray.push_back(static_cast<u8>(buffer));
        buffer = 0;
        bufferPosition = 0;
    }
}
