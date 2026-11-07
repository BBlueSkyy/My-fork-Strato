// SPDX-License-Identifier: MPL-2.0
// Temporary, non-invasive guest/GPU progress counters for BOTW freeze diagnosis.
// Remove these counters before merging an unrelated production change.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace skyline::diag {
    inline constexpr std::size_t MaxTrackedGuestThreads{128};

    // SVC entries show guest kernel transitions, not necessarily completed calls.
    // A running guest can execute indefinitely without issuing another SVC.
    inline std::atomic<std::uint64_t> guestSvcEntries{};
    inline std::array<std::atomic<std::uint64_t>, MaxTrackedGuestThreads> guestSvcByThread{};
    inline std::array<std::atomic<std::uint32_t>, MaxTrackedGuestThreads> lastSvcId{};

    // Submission and completion are counted separately to expose GPFIFO backlog.
    // These count pushbuffer entries, not rendered frames.
    inline std::atomic<std::uint64_t> gpfifoEnqueued{};
    inline std::atomic<std::uint64_t> gpfifoProcessed{};
}
