// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace skyline::kernel::diagnostic {
    struct ContextSample {
        uint64_t pc{}, sp{};
        std::array<uint64_t, 31> regs{};
        bool guest{};
    };

    // One outstanding request per thread. An unanswered request is never
    // recycled on a timeout: a delayed signal must retain its original owner.
    class ContextMailbox {
        enum Phase : uint32_t { Idle, Requested, Writing, Ready, Reading };
        static_assert(std::atomic<uint32_t>::is_always_lock_free);
        std::atomic<uint32_t> phase{Idle};
        ContextSample sample{};

      public:
        bool Request() noexcept {
            uint32_t expected{Idle};
            return phase.compare_exchange_strong(expected, Requested, std::memory_order_acq_rel);
        }

        // Only valid when signal delivery definitely failed, never on a timer.
        bool CancelUnsent() noexcept {
            uint32_t expected{Requested};
            return phase.compare_exchange_strong(expected, Idle, std::memory_order_acq_rel);
        }

        // Signal-side: no allocation, locks, logging or guest-memory accesses.
        bool Publish(const ContextSample &value) noexcept {
            uint32_t expected{Requested};
            if (!phase.compare_exchange_strong(expected, Writing, std::memory_order_acquire))
                return false;
            sample = value;
            phase.store(Ready, std::memory_order_release);
            return true;
        }

        bool Consume(ContextSample &value) noexcept {
            uint32_t expected{Ready};
            if (!phase.compare_exchange_strong(expected, Reading, std::memory_order_acquire))
                return false;
            value = sample;
            phase.store(Idle, std::memory_order_release);
            return true;
        }
    };
}
