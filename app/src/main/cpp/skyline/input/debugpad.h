// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <mutex>
#include "sections/DebugPad.h"

namespace skyline::input {
    /**
     * Publishes the disconnected debug device. An activated device still needs
     * samples: singular SDK readers leave the caller's buffer untouched when
     * the ring is empty. DebugPad has no host input binding in Strato.
     */
    class DebugPadManager {
      private:
        DebugPadSection &section;
        std::mutex mutex;
        bool activated{};

        void Publish(u64 timestamp) {
            const auto samplingNumber{section.entries[section.header.currentEntry].localTimestamp + 1};
            const auto next{(section.header.currentEntry + 1) % constant::HidEntryCount};
            auto &entry{section.entries[next]};
            const auto marker{samplingNumber << 1};
            __atomic_store_n(&entry.globalTimestamp, marker | 1, __ATOMIC_SEQ_CST);
            // A release store alone does not order subsequent payload writes
            // after the in-progress marker on AArch64.
            __atomic_thread_fence(__ATOMIC_RELEASE);
            entry.localTimestamp = samplingNumber;
            entry.attributes = 0;
            entry.buttons = 0;
            entry.rightX = 0;
            entry.rightY = 0;
            entry.leftX = 0;
            entry.leftY = 0;
            __atomic_store_n(&entry.globalTimestamp, marker, __ATOMIC_RELEASE);

            section.header.timestamp = timestamp;
            section.header.entryCount = constant::HidEntryCount;
            const auto count{std::min<u64>(section.header.maxEntry + 1, constant::HidEntryCount - 1)};
            // Publish metadata only after the new disconnected payload is complete.
            __atomic_store_n(&section.header.currentEntry, next, __ATOMIC_RELEASE);
            __atomic_store_n(&section.header.maxEntry, count, __ATOMIC_RELEASE);
        }

      public:
        explicit DebugPadManager(DebugPadSection &section) : section{section} {}

        void Activate(u64 timestamp) {
            std::scoped_lock lock{mutex};
            if (activated)
                return;
            Publish(timestamp);
            activated = true;
        }

        void UpdateSharedMemory(u64 timestamp) {
            std::scoped_lock lock{mutex};
            if (activated)
                Publish(timestamp);
        }
    };
}
