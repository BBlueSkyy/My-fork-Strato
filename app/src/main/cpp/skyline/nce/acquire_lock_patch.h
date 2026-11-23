// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace skyline::nce {
    // An unfenced test-and-set lock allows subsequent queue reads to be
    // reordered before the lock is acquired on some ARMv8 hosts. Rewrite only
    // the exact LDXR/CBNZ/STXR/CBNZ retry shape to use LDAXR. An immediate
    // DMB already supplies acquire ordering and must be left unchanged.
    inline std::size_t PatchUnfencedLockAcquires(std::uint8_t *text, std::size_t size) {
        std::size_t patched{};
        for (std::size_t offset{}; offset + 4 * sizeof(std::uint32_t) <= size; offset += sizeof(std::uint32_t)) {
            std::uint32_t words[4];
            std::memcpy(words, text + offset, sizeof(words));
            const auto [load, retryLoad, store, retryStore] = words;
            if ((load & 0xBFFFFC00u) != 0x885F7C00u)
                continue; // Only plain 32/64-bit LDXR, not LDAXR.

            const std::uint32_t resultRegister{load & 31u};
            const std::uint32_t baseRegister{(load >> 5) & 31u};
            const std::uint32_t statusRegister{(store >> 16) & 31u};
            const std::uint32_t storedRegister{store & 31u};
            const auto branchesBack = [](std::uint32_t branch, std::uint32_t reg, std::uint32_t distance) {
                return (branch & 0x7F00001Fu) == (0x35000000u | reg) &&
                       ((branch >> 5) & 0x7FFFFu) == (0x80000u - distance);
            };
            if (!branchesBack(retryLoad, resultRegister, 1) ||
                !branchesBack(retryStore, statusRegister, 3) ||
                (store & 0xBFE0FC00u) != 0x88007C00u ||
                ((load ^ store) & (1u << 30)) != 0 ||
                ((load & 0x40000000u) && !(retryLoad & 0x80000000u)) ||
                ((store >> 5) & 31u) != baseRegister ||
                resultRegister == 31u ||
                resultRegister == baseRegister ||
                storedRegister == 31u ||
                storedRegister == resultRegister ||
                statusRegister == 31u ||
                statusRegister == baseRegister ||
                statusRegister == storedRegister)
                continue;

            if (offset + 5 * sizeof(std::uint32_t) <= size) {
                std::uint32_t following{};
                std::memcpy(&following, text + offset + 4 * sizeof(std::uint32_t), sizeof(following));
                if (following == 0xD5033BBFu || following == 0xD50339BFu)
                    continue; // DMB ISH / ISHLD: already ordered.
            }

            const std::uint32_t acquireLoad{load | 0x8000u};
            std::memcpy(text + offset, &acquireLoad, sizeof(acquireLoad));
            ++patched;
        }
        return patched;
    }
}
