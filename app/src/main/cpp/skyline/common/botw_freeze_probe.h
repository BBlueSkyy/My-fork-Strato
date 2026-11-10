// SPDX-License-Identifier: MPL-2.0
// Temporary, non-invasive guest/GPU progress counters for BOTW freeze diagnosis.
// Remove these counters before merging an unrelated production change.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace skyline::diag {
    inline constexpr std::uint64_t BotwProgramId{0x01007EF00011E000ULL};
    inline std::atomic<bool> botwProbesEnabled{};
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    // Set after loading the NPDM, before starting any guest thread. Signal
    // handlers only read this lock-free flag, never process/loader objects.
    inline void ConfigureBotwProbes(std::uint64_t programId) noexcept {
        botwProbesEnabled.store(programId == BotwProgramId, std::memory_order_relaxed);
    }

    inline bool BotwProbesEnabled() noexcept {
        return botwProbesEnabled.load(std::memory_order_relaxed);
    }

    inline void IncrementBotwCounter(std::atomic<std::uint64_t> &counter,
                                     std::uint64_t count = 1) noexcept {
        if (BotwProbesEnabled())
            counter.fetch_add(count, std::memory_order_relaxed);
    }

    inline constexpr std::size_t MaxTrackedGuestThreads{128};

    // SVC entries show guest kernel transitions, not necessarily completed calls.
    // A running guest can execute indefinitely without issuing another SVC.
    inline std::atomic<std::uint64_t> guestSvcEntries{};
    inline std::array<std::atomic<std::uint64_t>, MaxTrackedGuestThreads> guestSvcByThread{};
    inline std::array<std::atomic<std::uint32_t>, MaxTrackedGuestThreads> lastSvcId{};

    inline void RecordBotwSvc(std::size_t id, std::uint32_t svcId) noexcept {
        if (!BotwProbesEnabled())
            return;
        guestSvcEntries.fetch_add(1, std::memory_order_relaxed);
        if (id < MaxTrackedGuestThreads) {
            guestSvcByThread[id].fetch_add(1, std::memory_order_relaxed);
            lastSvcId[id].store(svcId, std::memory_order_relaxed);
        }
    }

    // Submission and completion are counted separately to expose GPFIFO backlog.
    // These count pushbuffer entries, not rendered frames.
    inline std::atomic<std::uint64_t> gpfifoEnqueued{};
    inline std::atomic<std::uint64_t> gpfifoProcessed{};

    // Host interconnect 3D operations: attempts to draw, not necessarily
    // successfully executed GPU commands. Count *outside* retry lambdas to
    // avoid double-counting transient texture-backing retries.
    inline std::atomic<std::uint64_t> drawDirectCalls{};
    inline std::atomic<std::uint64_t> drawIndirectCalls{};
    inline std::atomic<std::uint64_t> drawTextureCalls{};
    inline std::atomic<std::uint64_t> drawInlineIndexCalls{};
    inline std::atomic<std::uint64_t> clearCalls{};
}
