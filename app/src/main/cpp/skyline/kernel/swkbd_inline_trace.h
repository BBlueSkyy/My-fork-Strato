// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace skyline::kernel::diagnostic {
    constexpr std::size_t NoSwkbdTraceThread{std::numeric_limits<std::size_t>::max()};
    constexpr std::uint32_t MaxSwkbdPostCmd2460Svcs{32};

    enum class SwkbdPostCmd2460Phase : std::uint8_t {
        Idle,
        AwaitingSendSyncReturn,
        Active,
    };

    inline std::atomic_size_t swkbdPostCmd2460Thread{NoSwkbdTraceThread};
    inline std::atomic_uint8_t swkbdPostCmd2460Phase{static_cast<std::uint8_t>(SwkbdPostCmd2460Phase::Idle)};
    inline std::atomic_uint32_t swkbdPostCmd2460Sequence{};

    inline void ArmSwkbdPostCmd2460Trace(std::size_t threadId) {
        swkbdPostCmd2460Sequence.store(0, std::memory_order_relaxed);
        swkbdPostCmd2460Thread.store(threadId, std::memory_order_release);
        swkbdPostCmd2460Phase.store(
            static_cast<std::uint8_t>(SwkbdPostCmd2460Phase::AwaitingSendSyncReturn),
            std::memory_order_release);
    }

    inline bool ActivateSwkbdPostCmd2460Trace(std::size_t threadId) {
        if (swkbdPostCmd2460Thread.load(std::memory_order_acquire) != threadId)
            return false;

        auto expected{static_cast<std::uint8_t>(SwkbdPostCmd2460Phase::AwaitingSendSyncReturn)};
        return swkbdPostCmd2460Phase.compare_exchange_strong(
            expected, static_cast<std::uint8_t>(SwkbdPostCmd2460Phase::Active),
            std::memory_order_acq_rel);
    }

    inline bool IsSwkbdPostCmd2460TraceActive(std::size_t threadId) {
        return swkbdPostCmd2460Thread.load(std::memory_order_acquire) == threadId &&
               swkbdPostCmd2460Phase.load(std::memory_order_acquire) ==
                   static_cast<std::uint8_t>(SwkbdPostCmd2460Phase::Active);
    }

    inline std::uint32_t BeginSwkbdPostCmd2460Svc(std::size_t threadId) {
        if (!IsSwkbdPostCmd2460TraceActive(threadId))
            return 0;

        const auto sequence{swkbdPostCmd2460Sequence.fetch_add(1, std::memory_order_acq_rel) + 1};
        if (sequence > MaxSwkbdPostCmd2460Svcs) {
            swkbdPostCmd2460Thread.store(NoSwkbdTraceThread, std::memory_order_release);
            swkbdPostCmd2460Phase.store(
                static_cast<std::uint8_t>(SwkbdPostCmd2460Phase::Idle),
                std::memory_order_release);
            return 0;
        }
        return sequence;
    }

    inline void FinishSwkbdPostCmd2460Svc(std::size_t threadId, std::uint32_t sequence) {
        if (sequence != MaxSwkbdPostCmd2460Svcs ||
            swkbdPostCmd2460Thread.load(std::memory_order_acquire) != threadId)
            return;

        swkbdPostCmd2460Thread.store(NoSwkbdTraceThread, std::memory_order_release);
        swkbdPostCmd2460Phase.store(
            static_cast<std::uint8_t>(SwkbdPostCmd2460Phase::Idle),
            std::memory_order_release);
    }
}
