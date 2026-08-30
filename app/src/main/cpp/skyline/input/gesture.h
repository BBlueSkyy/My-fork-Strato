// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "shared_mem.h"

namespace skyline::input {
    /**
     * @brief Recognizes high-level gestures from touch-screen samples and publishes them to HID shared memory
     */
    class GestureManager {
      private:
        static constexpr size_t MaxGesturePoints{4};

        struct Sample {
            std::array<GesturePoint, MaxGesturePoints> points{};
            size_t pointCount{};
            GesturePoint midpoint{};
            i64 contextNumber{};
            float averageRadius{};
            float angle{};
        };

        GestureSection &section;
        std::mutex mutex;
        bool activated{};
        u32 basicGestureId{};

        Sample previousSample{};
        GestureStateData previousState{};
        i64 lastUpdateTimestamp{};
        i64 lastTapTimestamp{};
        i64 lastPanDuration{};
        bool forceUpdate{true};
        bool pressAndTapEnabled{};

        static Sample ReadSample(const TouchScreenState &touchState);
        bool NeedsUpdate(const Sample &sample, i64 timestamp) const;
        GestureStateData Recognize(Sample &sample, i64 timestamp);
        void Reset();
        void WriteNextEntry(const GestureStateData &state, i64 timestamp);

      public:
        explicit GestureManager(input::HidSharedMemory *hid);

        /**
         * @brief Activates gesture reporting for the specified basic-gesture context
         */
        void Activate(u32 basicGestureId);

        /**
         * @brief Processes the latest touch-screen state
         * @param timestamp Monotonic host timestamp in nanoseconds
         */
        void Update(const TouchScreenState &touchState, i64 timestamp);
    };
}
