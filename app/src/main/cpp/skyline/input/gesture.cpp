// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include "gesture.h"

namespace skyline::input {
    namespace {
        constexpr float SwipeThreshold{400.0F};
        constexpr float PinchThreshold{0.5F};
        constexpr float RotationThreshold{0.015F};
        constexpr i64 PressDelayNs{500'000'000};
        constexpr i64 DoubleTapDelayNs{350'000'000};

        u64 NanosecondsToGuestTicks(i64 nanoseconds) {
            constexpr u64 NanosecondsPerSecond{1'000'000'000};
            constexpr u64 GuestClockFrequency{19'200'000};
            if (nanoseconds <= 0)
                return 0;

            const auto timestamp{static_cast<u64>(nanoseconds)};
            return (timestamp / NanosecondsPerSecond) * GuestClockFrequency +
                   ((timestamp % NanosecondsPerSecond) * GuestClockFrequency) / NanosecondsPerSecond;
        }

        GesturePoint Subtract(GesturePoint lhs, GesturePoint rhs) {
            return {
                .x = lhs.x - rhs.x,
                .y = lhs.y - rhs.y,
            };
        }

        float Length(GesturePoint point) {
            return std::hypot(static_cast<float>(point.x), static_cast<float>(point.y));
        }

        float Seconds(i64 duration) {
            constexpr float NanosecondsPerSecond{1'000'000'000.0F};
            return static_cast<float>(std::max<i64>(duration, 1)) / NanosecondsPerSecond;
        }

        GestureDirection GetSwipeDirection(GesturePoint delta) {
            if (std::abs(delta.x) > std::abs(delta.y))
                return delta.x > 0 ? GestureDirection::Right : GestureDirection::Left;

            if (delta.y != 0)
                return delta.y > 0 ? GestureDirection::Down : GestureDirection::Up;

            return GestureDirection::None;
        }
    }

    GestureManager::GestureManager(input::HidSharedMemory *hid) : section{hid->gesture} {}

    GestureManager::Sample GestureManager::ReadSample(const TouchScreenState &touchState) {
        Sample sample{};
        const auto touchCount{std::min<size_t>(touchState.touchCount, touchState.data.size())};

        i64 totalX{};
        i64 totalY{};
        for (size_t index{}; index < touchCount && sample.pointCount < MaxGesturePoints; index++) {
            const auto &touch{touchState.data[index]};
            if (touch.attribute.end)
                continue;

            auto &point{sample.points[sample.pointCount++]};
            point = {
                .x = static_cast<i32>(touch.positionX),
                .y = static_cast<i32>(touch.positionY),
            };
            totalX += point.x;
            totalY += point.y;
        }

        if (sample.pointCount == 0)
            return sample;

        sample.midpoint = {
            .x = static_cast<i32>(totalX / static_cast<i64>(sample.pointCount)),
            .y = static_cast<i32>(totalY / static_cast<i64>(sample.pointCount)),
        };

        for (size_t index{}; index < sample.pointCount; index++)
            sample.averageRadius += Length(Subtract(sample.points[index], sample.midpoint));
        sample.averageRadius /= static_cast<float>(sample.pointCount);

        if (sample.pointCount > 1) {
            const auto radial{Subtract(sample.midpoint, sample.points[0])};
            sample.angle = std::atan2(static_cast<float>(radial.y), static_cast<float>(radial.x));
        }

        return sample;
    }

    bool GestureManager::NeedsUpdate(const Sample &sample, i64 timestamp) const {
        if (forceUpdate || sample.pointCount != previousSample.pointCount)
            return true;

        if (sample.points != previousSample.points)
            return true;

        return pressAndTapEnabled && previousState.type == GestureType::Touch && sample.pointCount == 1 &&
               timestamp >= lastUpdateTimestamp && timestamp - lastUpdateTimestamp >= PressDelayNs;
    }

    GestureStateData GestureManager::Recognize(Sample &sample, i64 timestamp) {
        GestureStateData next{};
        next.samplingNumber = previousState.samplingNumber + 1;
        next.contextNumber = previousState.contextNumber;

        const i64 duration{timestamp >= lastUpdateTimestamp ? timestamp - lastUpdateTimestamp : 0};
        GestureType type{GestureType::Idle};

        if (sample.pointCount != 0) {
            if (previousSample.pointCount == 0) {
                next.contextNumber++;
                type = GestureType::Touch;
                if (previousState.type != GestureType::Cancel) {
                    next.attributes.newTouch = 1;
                    pressAndTapEnabled = true;
                }
            } else if (sample.pointCount != previousSample.pointCount) {
                type = GestureType::Cancel;
                pressAndTapEnabled = false;
                forceUpdate = true;
                sample = {};
                sample.contextNumber = next.contextNumber;
            } else if (sample.points != previousSample.points) {
                type = GestureType::Pan;
                next.delta = Subtract(sample.midpoint, previousState.position);
                next.velocityX = static_cast<float>(next.delta.x) / Seconds(duration);
                next.velocityY = static_cast<float>(next.delta.y) / Seconds(duration);
                lastPanDuration = duration;

                if (sample.pointCount > 1 && previousSample.averageRadius > 0.0F &&
                    std::abs(sample.averageRadius - previousSample.averageRadius) > PinchThreshold) {
                    type = GestureType::Pinch;
                    next.scale = sample.averageRadius / previousSample.averageRadius;
                }

                if (sample.pointCount > 1) {
                    const float angleDelta{std::remainder(sample.angle - previousSample.angle,
                                                          2.0F * std::numbers::pi_v<float>)};
                    if (std::abs(angleDelta) > RotationThreshold) {
                        type = GestureType::Rotate;
                        next.scale = 0.0F;
                        next.rotationAngle = angleDelta * 180.0F / std::numbers::pi_v<float>;
                    }
                }
            } else if (previousState.type == GestureType::Touch) {
                type = GestureType::Press;
            }
        } else if (previousSample.pointCount != 0) {
            switch (previousState.type) {
                case GestureType::Touch:
                    if (pressAndTapEnabled) {
                        type = GestureType::Tap;
                        sample = previousSample;
                        forceUpdate = true;
                        if (lastTapTimestamp != 0 && timestamp >= lastTapTimestamp &&
                            timestamp - lastTapTimestamp < DoubleTapDelayNs)
                            next.attributes.doubleTap = 1;
                        lastTapTimestamp = timestamp;
                    } else {
                        type = GestureType::Cancel;
                        forceUpdate = true;
                    }
                    break;
                case GestureType::Pan: {
                    const auto durationSeconds{Seconds(lastPanDuration + duration)};
                    next.velocityX = static_cast<float>(previousState.delta.x) / durationSeconds;
                    next.velocityY = static_cast<float>(previousState.delta.y) / durationSeconds;

                    if (std::hypot(next.velocityX, next.velocityY) > SwipeThreshold) {
                        type = GestureType::Swipe;
                        next.delta = previousState.delta;
                        next.direction = GetSwipeDirection(next.delta);
                        sample = previousSample;
                    } else {
                        type = GestureType::Complete;
                        next.velocityX = 0.0F;
                        next.velocityY = 0.0F;
                    }
                    forceUpdate = true;
                    break;
                }
                case GestureType::Press:
                case GestureType::Tap:
                case GestureType::Swipe:
                case GestureType::Pinch:
                case GestureType::Rotate:
                    type = GestureType::Complete;
                    forceUpdate = true;
                    break;
                default:
                    break;
            }
        } else if (previousState.type == GestureType::Complete || previousState.type == GestureType::Cancel) {
            next.contextNumber++;
        }

        sample.contextNumber = next.contextNumber;
        next.type = type;
        next.position = sample.midpoint;
        next.pointCount = static_cast<i32>(sample.pointCount);
        next.points = sample.points;

        previousSample = sample;
        previousState = next;
        lastUpdateTimestamp = timestamp;
        return next;
    }

    void GestureManager::Reset() {
        section = {};
        section.header.entryCount = constant::HidEntryCount;
        previousSample = {};
        previousState = {};
        lastUpdateTimestamp = 0;
        lastTapTimestamp = 0;
        lastPanDuration = 0;
        forceUpdate = true;
        pressAndTapEnabled = false;
    }

    void GestureManager::WriteNextEntry(const GestureStateData &state, i64 timestamp) {
        section.header.timestamp = NanosecondsToGuestTicks(timestamp);
        section.header.entryCount = constant::HidEntryCount;
        section.header.maxEntry = std::min<u64>(section.header.maxEntry + 1, constant::HidEntryCount - 1);
        section.header.currentEntry = (section.header.currentEntry + 1) % constant::HidEntryCount;

        auto &entry{section.entries[section.header.currentEntry]};
        std::atomic_ref<u64> marker{entry.globalTimestamp};
        const auto completedMarker{static_cast<u64>(state.samplingNumber) << 1};
        marker.store(completedMarker | 1, std::memory_order_relaxed);
        entry.data = state;
        marker.store(completedMarker, std::memory_order_release);
    }

    void GestureManager::Activate(u32 newBasicGestureId) {
        std::scoped_lock lock{mutex};
        if (activated && basicGestureId == newBasicGestureId)
            return;

        activated = true;
        basicGestureId = newBasicGestureId;
        Reset();
        WriteNextEntry(previousState, 0);
    }

    void GestureManager::Update(const TouchScreenState &touchState, i64 timestamp) {
        std::scoped_lock lock{mutex};
        if (!activated)
            return;

        auto sample{ReadSample(touchState)};
        if (!NeedsUpdate(sample, timestamp))
            return;

        forceUpdate = false;
        WriteNextEntry(Recognize(sample, timestamp), timestamp);
    }
}
