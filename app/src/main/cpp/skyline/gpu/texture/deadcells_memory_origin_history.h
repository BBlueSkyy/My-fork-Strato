// SPDX-License-Identifier: MPL-2.0
// Bounded metadata history for the experimental Dead Cells provenance probe.
// Pure C++: no Android, Vulkan, logging or guest memory reads.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace skyline::gpu::deadcells_probe::origin {
    // Half-open ranges; the subtraction formulation cannot overflow even when
    // a malicious/invalid end address would wrap in naive start + size math.
    [[nodiscard]] constexpr bool Overlaps(std::uint64_t first, std::size_t firstSize,
                                          std::uint64_t second, std::size_t secondSize) noexcept {
        if (!first || !second || !firstSize || !secondSize)
            return false;
        return first <= second ? (second - first) < firstSize :
                                 (first - second) < secondSize;
    }

    struct Event {
        std::uint64_t serial{}; // Shared host event sequence, not a GPU completion stamp.
        std::uint64_t timestampUs{};
        const char *kind{}; // Caller supplies a static string literal.
        std::uintptr_t hostStart{}; // Translated process pointer, optional.
        std::uint64_t guestStart{}; // Guest virtual address, optional.
        std::uint64_t gpuIova{}; // GMMU IOVA, optional.
        std::size_t size{};
        std::uint32_t handle{}; // NvMap handle, if known.
    };

    template<std::size_t Capacity>
    class History {
        static_assert(Capacity > 0);
        std::array<Event, Capacity> records{};
        std::uint64_t total{};

      public:
        void Reset() noexcept {
            records.fill({});
            total = 0;
        }

        [[nodiscard]] std::uint64_t Total() const noexcept { return total; }
        [[nodiscard]] std::uint64_t Overwritten() const noexcept {
            return total > Capacity ? total - Capacity : 0;
        }
        [[nodiscard]] constexpr std::size_t Limit() const noexcept { return Capacity; }

        void Record(Event value) noexcept {
            if (!value.size || (!value.hostStart && !value.guestStart))
                return;
            records[static_cast<std::size_t>(total % Capacity)] = value;
            ++total;
        }

        // Newest-first. Reports each event once even if host and guest spans
        // both overlap. Only metadata that is still in the ring is searchable.
        template<typename Callback>
        void ForEachOverlap(std::uintptr_t host, std::uint64_t guest,
                            std::size_t size, Callback callback) const {
            if (!size || (!host && !guest)) return;
            const auto available = total < Capacity ? static_cast<std::size_t>(total) : Capacity;
            for (std::size_t i = 0; i < available; ++i) {
                const auto &entry = records[static_cast<std::size_t>((total - i - 1) % Capacity)];
                const bool byHost = host && entry.hostStart &&
                    Overlaps(host, size, entry.hostStart, entry.size);
                const bool byGuest = guest && entry.guestStart &&
                    Overlaps(guest, size, entry.guestStart, entry.size);
                if (byHost || byGuest) callback(entry, byHost, byGuest);
            }
        }
    };
}
