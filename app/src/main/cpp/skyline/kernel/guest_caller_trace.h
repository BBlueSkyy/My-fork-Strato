// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace skyline::kernel::diagnostic {
    constexpr std::size_t NoGuestCallerTraceThread{std::numeric_limits<std::size_t>::max()};
    constexpr std::uint32_t MaxGuestCallerSvcs{64};

    enum class GuestCallerTracePhase : std::uint8_t {
        Idle,
        Reserving,
        AwaitingCmd2460Return,
        Active,
    };

    inline std::atomic_uint32_t swkbdIndirectAccessorCount{};
    inline std::atomic_uint8_t guestCallerTracePhase{static_cast<std::uint8_t>(GuestCallerTracePhase::Idle)};
    inline std::atomic_size_t guestCallerTraceThread{NoGuestCallerTraceThread};
    inline std::atomic_uint32_t guestCallerTraceGeneration{};
    inline std::atomic_uint32_t guestCallerSvcSequence{};

    inline void RegisterSwkbdIndirectAccessor() {
        swkbdIndirectAccessorCount.fetch_add(1, std::memory_order_acq_rel);
    }

    inline void UnregisterSwkbdIndirectAccessor() {
        auto current{swkbdIndirectAccessorCount.load(std::memory_order_acquire)};
        while (current &&
               !swkbdIndirectAccessorCount.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel)) {}
    }

    inline std::uint32_t ArmGuestCallerTrace(std::size_t threadId) {
        if (!swkbdIndirectAccessorCount.load(std::memory_order_acquire))
            return 0;

        auto expected{static_cast<std::uint8_t>(GuestCallerTracePhase::Idle)};
        if (!guestCallerTracePhase.compare_exchange_strong(
                expected, static_cast<std::uint8_t>(GuestCallerTracePhase::Reserving),
                std::memory_order_acq_rel))
            return 0;

        guestCallerTraceThread.store(threadId, std::memory_order_release);
        guestCallerSvcSequence.store(0, std::memory_order_relaxed);
        const auto generation{guestCallerTraceGeneration.fetch_add(1, std::memory_order_acq_rel) + 1};
        guestCallerTracePhase.store(
            static_cast<std::uint8_t>(GuestCallerTracePhase::AwaitingCmd2460Return),
            std::memory_order_release);
        return generation;
    }

    inline bool ActivateGuestCallerTraceAfterCmd2460(std::size_t threadId) {
        if (guestCallerTraceThread.load(std::memory_order_acquire) != threadId)
            return false;

        auto expected{static_cast<std::uint8_t>(GuestCallerTracePhase::AwaitingCmd2460Return)};
        return guestCallerTracePhase.compare_exchange_strong(
            expected, static_cast<std::uint8_t>(GuestCallerTracePhase::Active),
            std::memory_order_acq_rel);
    }

    inline std::uint32_t BeginGuestCallerSvc(std::size_t threadId) {
        if (guestCallerTraceThread.load(std::memory_order_acquire) != threadId ||
            guestCallerTracePhase.load(std::memory_order_acquire) !=
                static_cast<std::uint8_t>(GuestCallerTracePhase::Active))
            return 0;

        const auto sequence{guestCallerSvcSequence.fetch_add(1, std::memory_order_acq_rel) + 1};
        if (sequence > MaxGuestCallerSvcs) {
            guestCallerTraceThread.store(NoGuestCallerTraceThread, std::memory_order_release);
            guestCallerTracePhase.store(
                static_cast<std::uint8_t>(GuestCallerTracePhase::Idle),
                std::memory_order_release);
            return 0;
        }
        return sequence;
    }

    inline void FinishGuestCallerSvc(std::size_t threadId, std::uint32_t sequence) {
        if (!sequence || sequence != MaxGuestCallerSvcs ||
            guestCallerTraceThread.load(std::memory_order_acquire) != threadId)
            return;

        guestCallerTraceThread.store(NoGuestCallerTraceThread, std::memory_order_release);
        guestCallerTracePhase.store(
            static_cast<std::uint8_t>(GuestCallerTracePhase::Idle),
            std::memory_order_release);
    }

    inline std::uint32_t CurrentGuestCallerGeneration() {
        return guestCallerTraceGeneration.load(std::memory_order_acquire);
    }
}
