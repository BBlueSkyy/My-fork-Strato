// SPDX-License-Identifier: MPL-2.0
// One-shot Vulkan recording-slot diagnostic for BOTW visual softlocks.
// Diagnostic only: command recording is not proof of GPU execution or changed pixels.
#pragma once

#include <atomic>
#include <cstdint>

namespace skyline::diag {
    inline constexpr std::uint32_t VkSampleMaxDraws{32};
    inline constexpr std::uint32_t VkSampleMaxPasses{12};
    enum class VkSlotSampleState : unsigned { Idle, Armed, Recording, Submitted };

    inline std::atomic<VkSlotSampleState> vkSlotSampleState{VkSlotSampleState::Idle};
    inline thread_local bool vkSlotSampleActive{};
    inline thread_local std::uint64_t vkSlotRecordedDraws{};
    inline thread_local std::uint64_t vkSlotRecordedPasses{};
    inline thread_local std::uint64_t vkSlotRecordedSubpasses{};

    inline bool ArmVkSlotSample() {
        auto expected{VkSlotSampleState::Idle};
        return vkSlotSampleState.compare_exchange_strong(
            expected, VkSlotSampleState::Armed, std::memory_order_acq_rel);
    }

    inline bool BeginVkSlotSample() {
        auto expected{VkSlotSampleState::Armed};
        if (!vkSlotSampleState.compare_exchange_strong(
                expected, VkSlotSampleState::Recording, std::memory_order_acq_rel))
            return false;
        vkSlotSampleActive = true;
        vkSlotRecordedDraws = 0;
        vkSlotRecordedPasses = 0;
        vkSlotRecordedSubpasses = 0;
        return true;
    }

    inline bool RecordVkSampleDraw() {
        if (!vkSlotSampleActive)
            return false;
        return ++vkSlotRecordedDraws <= VkSampleMaxDraws;
    }

    inline bool RecordVkSamplePass() {
        if (!vkSlotSampleActive)
            return false;
        return ++vkSlotRecordedPasses <= VkSampleMaxPasses;
    }

    inline void RecordVkSampleSubpass() {
        if (vkSlotSampleActive)
            ++vkSlotRecordedSubpasses;
    }

    inline void CompleteVkSlotSample() {
        if (!vkSlotSampleActive)
            return;
        vkSlotSampleActive = false;
        vkSlotSampleState.store(VkSlotSampleState::Submitted, std::memory_order_release);
    }
}
