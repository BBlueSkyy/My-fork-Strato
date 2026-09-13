// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "common.h"
#include "kernel/types/KSharedMemory.h"
#include "input/shared_mem.h"
#include "input/gesture.h"
#include "input/npad.h"
#include "input/touch.h"

namespace skyline::input {
    /**
     * @brief The Input class manages components responsible for translating host input to guest input
     */
    class Input {
      private:
        const DeviceState &state;
        mutable std::mutex appletResourceMutex;
        std::unordered_set<u64> appletResources;

      public:
        std::shared_ptr<kernel::type::KSharedMemory> kHid; //!< The kernel shared memory object for HID Shared Memory
        HidSharedMemory *hid; //!< A pointer to HID Shared Memory on the host

        NpadManager npad;
        TouchManager touch;
        GestureManager gesture;

        Input(const DeviceState &state);

        /** Registers the application-scoped HID state owned by an IAppletResource. */
        bool RegisterAppletResource(u64 aruid);

        /** Releases application-scoped HID state when its IAppletResource dies. */
        void UnregisterAppletResource(u64 aruid);

        /** Returns whether the ARUID currently owns a live IAppletResource. */
        bool IsAppletResourceRegistered(u64 aruid) const;

      private:
        std::thread updateThread; //!< A thread that handles delivering HID shared memory updates at a fixed rate

        /**
         * @brief The entry point for the update thread, this handles timing and delegation to the shared memory managers
         */
        void UpdateThread();
    };
}
