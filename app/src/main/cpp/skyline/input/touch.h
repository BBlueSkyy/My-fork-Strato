// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <jni.h>
#include "shared_mem.h"

namespace skyline::input {
    /*
     * @brief A description of a point being touched on the screen
     * @note All members are jint as it's treated as a jint array in Kotlin
     * @note This structure corresponds to TouchScreenStateData, see that for details
     */
    struct TouchScreenPoint {
        jint attribute;
        jint id;
        jint x;
        jint y;
        jint minor;
        jint major;
        jint angle;
    };

    /**
     * @brief This class is used to manage the shared memory responsible for touch-screen data
     */
    class TouchManager {
      private:
        const DeviceState &state;
        bool activated{};
        TouchScreenSection &section;

        std::recursive_mutex mutex;
        TouchScreenState screenState{}; //!< The current state of the touch screen
        std::array<uint8_t, 16> pointTimeout; //!< A frame timeout counter for each point which has ended (according to it's attribute), when it reaches 0 the point is removed from the screen
        uint32_t touchScreenWidth{1280};  //!< Resolution reported by the guest via SetTouchScreenResolution
        uint32_t touchScreenHeight{720}; 
      
     public:
        /**
         * @param hid A pointer to HID Shared Memory on the host
         */
        TouchManager(const DeviceState &state, input::HidSharedMemory *hid);

        void Activate();

        void SetState(span<TouchScreenPoint> touchPoints);
        /**
         * @brief Stores the touch screen resolution as reported by the guest
         */
        void SetResolution(uint32_t width, uint32_t height);
        /**
         * @brief Writes the current state of the touch screen to HID shared memory
         * @return A snapshot of the state written for use by gesture recognition
         */
        TouchScreenState UpdateSharedMemory();
    };
}
