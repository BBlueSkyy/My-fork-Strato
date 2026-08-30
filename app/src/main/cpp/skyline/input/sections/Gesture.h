// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <cstddef>
#include "common.h"

namespace skyline::input {
    /**
     * @brief A point reported by the gesture recognizer
     */
    struct GesturePoint {
        i32 x;
        i32 y;

        constexpr bool operator==(const GesturePoint &) const = default;
    };
    static_assert(sizeof(GesturePoint) == 0x8);

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#GestureType
     */
    enum class GestureType : u32 {
        Idle,
        Complete,
        Cancel,
        Touch,
        Press,
        Tap,
        Pan,
        Swipe,
        Pinch,
        Rotate,
    };

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#GestureDirection
     */
    enum class GestureDirection : u32 {
        None,
        Left,
        Up,
        Right,
        Down,
    };

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#GestureAttribute
     */
    union GestureAttribute {
        u32 raw{};
        struct {
            u32 _reserved0_ : 4;
            u32 newTouch : 1;
            u32 _reserved1_ : 3;
            u32 doubleTap : 1;
            u32 _reserved2_ : 23;
        };
    };
    static_assert(sizeof(GestureAttribute) == 0x4);

    /**
     * @brief The guest-visible nn::hid::GestureState payload
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#GestureDummyState
     */
    struct GestureStateData {
        i64 samplingNumber;
        i64 contextNumber;
        GestureType type;
        GestureDirection direction;
        GesturePoint position;
        GesturePoint delta;
        float velocityX;
        float velocityY;
        GestureAttribute attributes;
        float scale;
        float rotationAngle;
        i32 pointCount;
        std::array<GesturePoint, 4> points;
    };
    static_assert(sizeof(GestureStateData) == 0x60);
    static_assert(offsetof(GestureStateData, type) == 0x10);
    static_assert(offsetof(GestureStateData, points) == 0x40);

    /**
     * @brief Atomic-storage marker followed by the gesture payload
     */
    struct GestureState {
        u64 globalTimestamp;
        GestureStateData data;
    };
    static_assert(sizeof(GestureState) == 0x68);

    /**
     * @url https://switchbrew.org/wiki/HID_Shared_Memory#Gesture
     */
    struct GestureSection {
        CommonHeader header;
        std::array<GestureState, constant::HidEntryCount> entries;
        u64 _pad_[0x1F];
    };
    static_assert(sizeof(GestureSection) == 0x800);
}
