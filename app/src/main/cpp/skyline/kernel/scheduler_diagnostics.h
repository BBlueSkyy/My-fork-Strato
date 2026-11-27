// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace skyline::kernel::diagnostic {
    // RING-DIAG (temporary): what a guest thread is currently doing inside the kernel/HLE layer.
    // Written only by the owning thread at SVC entry/exit, read once by the one-shot ring-full dump.
    struct SvcDiag {
        std::atomic<uint64_t> count{}, enterNs{}, pc{}, x0{}, x1{}, x2{}, x3{}, x19{}, x20{}, x21{};
        std::atomic<uint32_t> id{};
        std::atomic<uint32_t> phase{}; //!< 0 outside, 1 inside SVC body, 2 post-SVC scheduler wait
        std::atomic<const char *> ipcName{};
        std::atomic<uint32_t> ipcCmd{};
        std::atomic<const char *> nvDevice{};
        std::atomic<uint32_t> nvFd{}, nvCmd{};
    };

    struct ContextSample {
        uint64_t pc{}, sp{};
        std::array<uint64_t, 31> regs{};
        bool guest{};
    };

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

        bool CancelUnsent() noexcept {
            uint32_t expected{Requested};
            return phase.compare_exchange_strong(expected, Idle, std::memory_order_acq_rel);
        }

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
