// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <cstddef>
#include "sections/DebugPad.h"
#include "sections/TouchScreen.h"
#include "sections/Mouse.h"
#include "sections/Keyboard.h"
#include "sections/BasicXpad.h"
#include "sections/Button.h"
#include "sections/InputDetector.h"
#include "sections/Npad.h"
#include "sections/Gesture.h"
#include "sections/ConsoleSixAxisSensor.h"

namespace skyline {
    namespace input {
        /**
         * @brief Global Npad condition state used by recent versions of nn::hid
         */
        struct NpadCondition {
            u32 _unknown_;
            u32 initialized;
            u32 holdType;
            u32 valid;
        };
        static_assert(sizeof(NpadCondition) == 0x10);

        /**
         * @url https://switchbrew.org/wiki/HID_Shared_Memory
         */
        struct HidSharedMemory {
            DebugPadSection debugPad; //!< The DebugPad section (https://switchbrew.org/wiki/HID_Shared_Memory#DebugPad)
            TouchScreenSection touchScreen; //!< The TouchScreen section (https://switchbrew.org/wiki/HID_Shared_Memory#TouchScreen)
            MouseSection mouse; //!< The Mouse section (https://switchbrew.org/wiki/HID_Shared_Memory#Mouse)
            KeyboardSection keyboard; //!< The Keyboard section (https://switchbrew.org/wiki/HID_Shared_Memory#Keyboard)
            std::array<BasicXpadSection, 4> xpad; //!< The XPad (XBOX Pad) section (https://switchbrew.org/wiki/HID_Shared_Memory#BasicXpad)
            ButtonSection homeButton; //!< The HomeButton section (https://switchbrew.org/wiki/HID_Shared_Memory#HomeButton)
            ButtonSection sleepButton; //!< The SleepButton section (https://switchbrew.org/wiki/HID_Shared_Memory#SleepButton)
            ButtonSection captureButton; //!< The CaptureButton section (https://switchbrew.org/wiki/HID_Shared_Memory#CaptureButton)
            std::array<InputDetectorSection, 16> inputDetector; //!< The InputDetector section (https://switchbrew.org/wiki/HID_Shared_Memory#InputDetector)
            u64 _pad0_[0x80 * 0x10]; //!< This was UniquePad in FW 5.0.0 and below but has been removed (https://switchbrew.org/wiki/HID_Shared_Memory#UniquePad)
            std::array<NpadSection, constant::NpadCount> npad; //!< The NPad (Nintendo Pad) section (https://switchbrew.org/wiki/HID_Shared_Memory#Npad)
            GestureSection gesture; //!< The Gesture section (https://switchbrew.org/wiki/HID_Shared_Memory#Gesture)
            ConsoleSixAxisSensorSection consoleSixAxisSensor; //!< The SixAxis (Gyroscope/Accelerometer) section on FW 5.0.0 and above (https://switchbrew.org/wiki/HID_Shared_Memory#ConsoleSixAxisSensor)
            std::array<u8, 0x19E0> _pad1_;
            MouseSection debugMouse;
            std::array<u8, 0x200> _pad2_;
            NpadCondition npadCondition;
            std::array<u8, 0x1DF0> _pad3_;
        };
        static_assert(offsetof(HidSharedMemory, debugMouse) == 0x3DC00);
        static_assert(offsetof(HidSharedMemory, npadCondition) == 0x3E200);
        static_assert(sizeof(HidSharedMemory) == 0x40000);
    }
}
