// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <span>

namespace skyline::fault_access {
    enum class Access { Read, Write, Unknown };

    // Routing requires a unique, well-formed Linux arm64 context record chain
    // and a userspace data fault. ESR must reside in __reserved;
    // never follow extra_context pointers or inspect the faulting instruction.
    inline Access Classify(std::span<const std::byte> records) {
        Access result{Access::Unknown};
        bool foundEsr{};
        size_t offset{};
        while (records.size() - offset >= 8) {
            uint32_t magic{}, size{};
            std::memcpy(&magic, records.data() + offset, sizeof(magic));
            std::memcpy(&size, records.data() + offset + 4, sizeof(size));
            if (!magic)
                return size ? Access::Unknown : result;
            if (size < 16 || size % 16 || size > records.size() - offset)
                return Access::Unknown;
            if (magic == 0x45535201) { // Linux arm64 ESR_MAGIC / esr_context.
                if (foundEsr || size != 16)
                    return Access::Unknown;
                foundEsr = true;
                uint64_t esr{};
                std::memcpy(&esr, records.data() + offset + 8, sizeof(esr));
                const auto exceptionClass{(esr >> 26) & 0x3f};
                const auto faultStatus{esr & 0x3f};
                // Cache maintenance, translation-table walks and invalid FARs
                // are not evidence of a CPU data load. Keep legacy handling.
                constexpr uint64_t Ambiguous{(1ULL << 8) | (1ULL << 7) | (1ULL << 10)};
                // Linux arm64 PROT_NONE clears PTE_VALID, so a trapped load
                // can have a translation syndrome despite SEGV_ACCERR. Read-only
                // pages instead cause permission faults on stores. MayWrite also
                // checks si_code so an unmapped address keeps legacy handling.
                const bool translation{faultStatus >= 0x04 && faultStatus <= 0x07};
                const bool permission{faultStatus >= 0x0c && faultStatus <= 0x0f};
                if (exceptionClass != 0x24 || (!translation && !permission) || (esr & Ambiguous))
                    return Access::Unknown;
                result = (esr & (1ULL << 6)) ? Access::Write : Access::Read;
            }
            offset += size;
        }
        return Access::Unknown; // Missing record-chain terminator.
    }

    inline bool MayWrite(int signal, int code, std::span<const std::byte> records) {
        // Unknowns retain the original conservative write routing.
        return signal != SIGSEGV || code != SEGV_ACCERR || Classify(records) != Access::Read;
    }
}
