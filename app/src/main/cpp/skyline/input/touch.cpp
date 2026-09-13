// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "touch.h"

namespace skyline::input {
    TouchManager::TouchManager(const DeviceState &state, input::HidSharedMemory *hid) : state(state), section(hid->touchScreen) {
        Activate(); // The touch screen is expected to be activated by default, commercial games are reliant on this behavior
    }

    void TouchManager::Activate() {
        std::scoped_lock lock{mutex};
        if (!activated) {
            activated = true;
            SetState({});
        }
    }
    
    void TouchManager::SetResolution(uint32_t width, uint32_t height) {
        std::scoped_lock lock{mutex};
        touchScreenWidth = std::max(width, 1U);
        touchScreenHeight = std::max(height, 1U);
    }

    void TouchManager::SetConfiguration(TouchScreenConfiguration configuration) {
        std::scoped_lock lock{mutex};
        switch (configuration.mode) {
            case TouchScreenMode::Finger:
            case TouchScreenMode::Heat2:
                mode = configuration.mode;
                break;
            default:
                mode = TouchScreenMode::UseSystemSetting;
                break;
        }
    }
   
    void TouchManager::SetState(span<TouchScreenPoint> touchPoints) {
        std::scoped_lock lock{mutex};

        std::array<TouchScreenStateData, 16> pendingEnded{};
        std::array<u8, 16> pendingTimeout{};
        size_t pendingCount{};
        for (size_t i{}; i < static_cast<size_t>(screenState.touchCount); ++i) {
            if (screenState.data[i].attribute.end && pointTimeout[i] > 0) {
                pendingEnded[pendingCount] = screenState.data[i];
                pendingTimeout[pendingCount++] = pointTimeout[i];
            }
        }

        screenState.touchCount = 0;
        screenState.data.fill({});
        pointTimeout.fill(0);

        const auto appendPoint = [&](const TouchScreenPoint &host) {
            if (screenState.touchCount >= static_cast<i32>(screenState.data.size()))
                return;

            const auto index{static_cast<size_t>(screenState.touchCount++)};
            auto &guest{screenState.data[index]};

            guest.attribute.raw = static_cast<u32>(host.attribute) & 0x3;
            guest.index = static_cast<u32>(std::max(host.id, 0));
            guest.positionX = static_cast<u32>(std::clamp(host.x, 0, static_cast<jint>(touchScreenWidth - 1)));
            guest.positionY = static_cast<u32>(std::clamp(host.y, 0, static_cast<jint>(touchScreenHeight - 1)));
            guest.minorAxis = static_cast<u32>(std::clamp(host.minor, 0, static_cast<jint>(touchScreenWidth)));
            guest.majorAxis = static_cast<u32>(std::clamp(host.major, 0, static_cast<jint>(touchScreenWidth)));
            guest.angle = std::clamp(host.angle, -90, 90);

            constexpr uint8_t TouchPointTimeout{3}; //!< The amount of frames an ended point is expected to be active before it is removed from the screen
            if (guest.attribute.end)
                pointTimeout[index] = TouchPointTimeout;
        };

        touchPoints = touchPoints.first(std::min(touchPoints.size(), screenState.data.size()));
        for (const auto &point : touchPoints)
            appendPoint(point);

        // Android removes a lifted pointer from the next MotionEvent. Preserve its
        // End sample independently of pointer-array reordering so the guest always
        // observes a complete begin/continue/end lifecycle for the same finger ID.
        for (size_t i{}; i < pendingCount && screenState.touchCount < static_cast<i32>(screenState.data.size()); ++i) {
            const auto duplicate{std::find_if(screenState.data.begin(), screenState.data.begin() + screenState.touchCount, [&](const auto &point) {
                return point.index == pendingEnded[i].index;
            })};
            if (duplicate != screenState.data.begin() + screenState.touchCount)
                continue;

            const auto index{static_cast<size_t>(screenState.touchCount++)};
            screenState.data[index] = pendingEnded[i];
            pointTimeout[index] = pendingTimeout[i];
        }
    }

    TouchScreenState TouchManager::UpdateSharedMemory() {
        std::scoped_lock lock{mutex};

        for (size_t i{}; i < static_cast<size_t>(screenState.touchCount);) {
            // Remove any touch points which have ended after they are timed out
            if (screenState.data[i].attribute.end) {
                auto &timeout{pointTimeout[i]};
                if (timeout > 0) {
                    // Tick the timeout counter
                    timeout--;
                    i++;
                } else {
                    // Erase the point from the screen
                    const auto last{static_cast<size_t>(screenState.touchCount - 1)};
                    if (i != last) {
                        // Move every point after the one being removed to fill the gap
                        for (size_t j{i + 1}; j <= last; j++) {
                            screenState.data[j - 1] = screenState.data[j];
                            pointTimeout[j - 1] = pointTimeout[j];
                        }
                    }
                    screenState.data[last] = {};
                    pointTimeout[last] = 0;
                    screenState.touchCount--;
                }
            } else
                i++;
        }

        if (!activated)
            return screenState;

        const auto &lastEntry{section.entries[section.header.currentEntry]};

        section.header.timestamp = util::GetTimeTicks();
        section.header.entryCount = constant::HidEntryCount;
        section.header.maxEntry = std::min<u64>(section.header.maxEntry + 1, constant::HidEntryCount - 1);
        section.header.currentEntry = (section.header.currentEntry + 1) % constant::HidEntryCount;

        const auto nextSamplingNumber{lastEntry.localTimestamp + 1};
        screenState.localTimestamp = nextSamplingNumber;
        for (size_t i{}; i < static_cast<size_t>(screenState.touchCount); ++i)
            screenState.data[i].timestamp = nextSamplingNumber;

        auto &entry{section.entries[section.header.currentEntry]};
        const auto completedMarker{nextSamplingNumber << 1};
        __atomic_store_n(&entry.globalTimestamp, completedMarker | 1, __ATOMIC_RELAXED);
        entry.localTimestamp = screenState.localTimestamp;
        entry.touchCount = screenState.touchCount;
        entry._reserved_ = 0;
        entry.data = screenState.data;
        __atomic_store_n(&entry.globalTimestamp, completedMarker, __ATOMIC_RELEASE);
        screenState.globalTimestamp = completedMarker;
        return screenState;
    }
}
