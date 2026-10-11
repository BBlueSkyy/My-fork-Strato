// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <mutex>
#include <utility>

namespace skyline::gpu::interconnect {
    /**
     * Serializes executable CopyOnly work for the lifetime of one submission.
     *
     * The lock is global because one draw may touch multiple TextureGroups and
     * must never submit or rotate its command slot merely to change groups.
     */
    class CopyOnlyRuntimeSerialization {
      private:
        static inline std::mutex globalMutex;
        std::unique_lock<std::mutex> lock;

      public:
        CopyOnlyRuntimeSerialization() = default;
        CopyOnlyRuntimeSerialization(const CopyOnlyRuntimeSerialization &) = delete;
        CopyOnlyRuntimeSerialization &operator=(const CopyOnlyRuntimeSerialization &) = delete;

        bool OwnsLock() const {
            return lock.owns_lock();
        }

        void Acquire() {
            if (!lock.owns_lock())
                lock = std::unique_lock{globalMutex};
        }

        bool TryAcquire() {
            if (lock.owns_lock())
                return true;
            std::unique_lock candidate{globalMutex, std::try_to_lock};
            if (!candidate.owns_lock())
                return false;
            lock = std::move(candidate);
            return true;
        }

        void Reset() {
            lock = {};
        }
    };
}
