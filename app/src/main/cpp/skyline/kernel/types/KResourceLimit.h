// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <atomic>
#include "KObject.h"

namespace skyline::kernel::type {
    enum class LimitableResource : u32 {
        Memory = 0,
        Threads = 1,
        Events = 2,
        TransferMemories = 3,
        Sessions = 4,
        Count = 5,
    };

    /**
     * @brief Process resource-limit object exposed by svcGetInfo(InfoType_ResourceLimit).
     *
     * Strato currently uses this object for Horizon-compatible query semantics. It does
     * not advertise svcSetResourceLimitLimitValue, so guests cannot mutate quotas that
     * the kernel does not yet enforce.
     */
    class KResourceLimit : public KObject {
      private:
        static constexpr size_t ResourceCount{static_cast<size_t>(LimitableResource::Count)};
        std::array<std::atomic<i64>, ResourceCount> limitValues{};
        std::array<std::atomic<i64>, ResourceCount> currentValues{};

        static constexpr size_t Index(LimitableResource resource) {
            return static_cast<size_t>(resource);
        }

      public:
        explicit KResourceLimit(const DeviceState &state) : KObject(state, KType::KResourceLimit) {}

        i64 GetLimitValue(LimitableResource resource) const {
            return limitValues.at(Index(resource)).load(std::memory_order_relaxed);
        }

        i64 GetCurrentValue(LimitableResource resource) const {
            return currentValues.at(Index(resource)).load(std::memory_order_relaxed);
        }

        void SetLimitValue(LimitableResource resource, i64 value) {
            limitValues.at(Index(resource)).store(value, std::memory_order_relaxed);
        }

        void SetCurrentValue(LimitableResource resource, i64 value) {
            currentValues.at(Index(resource)).store(value, std::memory_order_relaxed);
        }
    };
}
