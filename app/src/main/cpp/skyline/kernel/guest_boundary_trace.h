// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace skyline::kernel {
    /**
     * @brief Temporary diagnostic: records where guest threads park or call into services after the SWKBD image-memory query
     * @details The inline keyboard log ends right after the VI memory query, so it cannot tell a blocked guest thread from a guest
     * that simply never reaches its Calc update. Once armed, the first blocking SVC / IPC command of each (thread, kind, object)
     * is recorded. For a parked thread the last record is where it is waiting. The trace disarms itself after Capacity records,
     * so the hot-path cost afterwards is one relaxed atomic load. It never alters guest-visible behaviour.
     * @note Remove together with the [SWKBD] diagnostics once the Hitman cause is established
     */
    class GuestBoundaryTrace {
      public:
        enum class Kind : std::uint32_t { WaitSynchronization = 0x18, ArbitrateLock = 0x1A, WaitProcessWideKey = 0x1C, Ipc = 0x100 };
        enum class Decision { Inactive, Emit, Duplicate, Limit };
        static constexpr std::size_t Capacity{160};

        /** @brief Starts recording, only the first call has an effect */
        static bool Arm() {
            std::scoped_lock lock{Mutex()};
            if (State().armedOnce)
                return false;
            State().armedOnce = true;
            Armed().store(true, std::memory_order_release);
            return true;
        }

        static bool IsArmed() { return Armed().load(std::memory_order_relaxed); }

        /** @brief Dedupes on (thread, kind, object): `detail` and `extra` are for the log line only */
        static Decision Take(std::uint64_t thread, Kind kind, std::uint64_t object) {
            if (!IsArmed())
                return Decision::Inactive;
            std::scoped_lock lock{Mutex()};
            auto &state{State()};
            const Key key{thread, static_cast<std::uint32_t>(kind), object};
            for (std::size_t index{}; index < state.count; ++index)
                if (state.keys[index] == key)
                    return Decision::Duplicate;
            if (state.count == Capacity) {
                Armed().store(false, std::memory_order_release);
                return Decision::Limit;
            }
            state.keys[state.count++] = key;
            return Decision::Emit;
        }

        /** @brief Test hook: restores the pristine, unarmed state */
        static void ResetForTest() {
            std::scoped_lock lock{Mutex()};
            State() = {};
            Armed().store(false, std::memory_order_release);
        }

        static std::size_t Recorded() {
            std::scoped_lock lock{Mutex()};
            return State().count;
        }

      private:
        struct Key {
            std::uint64_t thread{};
            std::uint32_t kind{};
            std::uint64_t object{};
            bool operator==(const Key &) const = default;
        };
        struct Storage {
            std::array<Key, Capacity> keys{};
            std::size_t count{};
            bool armedOnce{};
        };

        static std::mutex &Mutex() { static std::mutex mutex; return mutex; }
        static Storage &State() { static Storage storage; return storage; }
        static std::atomic<bool> &Armed() { static std::atomic<bool> armed{false}; return armed; }
    };
}
