// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>

namespace skyline::nce::diagnostics::instructions {
    // This is a reader of a few PC-relative encodings, never an executor/patcher.
    constexpr int64_t SignExtend(uint32_t value, unsigned bits) {
        return static_cast<int64_t>(value) - ((value & (1U << (bits - 1))) ? (1LL << bits) : 0);
    }

    constexpr std::optional<uint64_t> DirectBranch(uint32_t word, uint64_t pc) {
        if ((word & 0x7C000000) != 0x14000000) // B or BL, not a conditional branch
            return {};
        return pc + SignExtend(word & 0x03FFFFFF, 26) * 4;
    }

    struct Reference {
        uint64_t address;
        bool pointerLoad;
    };

    // Only adjacent ADRP+ADD/LDR pairs using the same base register are inferred.
    // Other forms are left as raw instructions for offline analysis.
    constexpr std::optional<Reference> DataReference(uint32_t word, uint32_t next, uint64_t pc) {
        if ((word & 0x9F000000) != 0x10000000 && (word & 0x9F000000) != 0x90000000)
            return {};
        const auto immediate{SignExtend(((word >> 5) & 0x7FFFF) << 2 | ((word >> 29) & 3), 21)};
        if (!(word & 0x80000000)) // ADR
            return Reference{pc + immediate, false};
        if ((word & 31) == 31 || ((next >> 5) & 31) != (word & 31))
            return {};
        const uint64_t page{(pc & ~uint64_t{0xFFF}) + immediate * 4096};
        if ((next & 0xFFC00000) == 0x91000000) // ADD Xd, Xn, #imm12, LSL #0
            return Reference{page + ((next >> 10) & 0xFFF), false};
        if ((next & 0x3FC00000) == 0x39400000) { // LDR{B,H,W,X}, unsigned immediate
            const unsigned scale{next >> 30};
            return Reference{page + (((next >> 10) & 0xFFF) << scale), scale == 3};
        }
        return {};
    }
}
