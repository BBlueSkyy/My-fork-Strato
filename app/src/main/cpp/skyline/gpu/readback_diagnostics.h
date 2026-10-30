// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace skyline::gpu::readback_diagnostics {
    // Diagnostic branch only. Counts are cumulative callback attempts, not unique
    // resources or frames. Relaxed snapshots can straddle concurrent updates.
    enum Metric : size_t {
        ReadTrap, WriteTrap, NotGpuDirty, Retry, ReadSync, WriteSync,
        Disabled, BelowThreshold, Aliased, Freed, FastEligible, FastTaken, FastWrites,
        WaitCount, WaitNs, MetricCount
    };
    using Values = std::array<uint64_t, MetricCount>;
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    struct Counters {
        std::array<std::atomic<uint64_t>, MetricCount> values{};
        void Add(Metric metric, uint64_t amount = 1) {
            values[metric].fetch_add(amount, std::memory_order_relaxed);
        }
        void Wait(uint64_t ns) { Add(WaitCount); Add(WaitNs, ns); }
        void Decision(bool enabled, bool threshold, bool aliases, bool freed) {
            auto reason{!enabled ? Disabled : !threshold ? BelowThreshold : aliases ? Aliased : freed ? Freed : FastEligible};
            Add(reason);
        }
        Values Snapshot() const {
            Values result{};
            for (size_t i{}; i < MetricCount; ++i)
                result[i] = values[i].load(std::memory_order_relaxed);
            return result;
        }
    };
    inline Counters buffers, textures;
    inline uint64_t NowNs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    // Diagnostic wall-clock phase totals. They are not GPU timestamps and
    // separate threads' measurements can overlap.
    enum FrameMetric : size_t {
        PresentedFrames, PresentCpuNs, GuestFenceNs, SwapchainFenceNs,
        AcquireSwapchainNs, TextureSyncNs, DisplayCopyNs, QueuePresentNs,
        RecordedSlots, RecordWorkNs, SubmitNs, AcquireSlots, AcquireSlotWaitNs,
        CompletedFences, CompletionWaitNs, FrameMetricCount
    };
    using FrameValues = std::array<uint64_t, FrameMetricCount>;
    struct FrameCounters {
        std::array<std::atomic<uint64_t>, FrameMetricCount> values{};
        void Add(FrameMetric metric, uint64_t amount = 1) {
            values[metric].fetch_add(amount, std::memory_order_relaxed);
        }
        FrameValues Snapshot() const {
            FrameValues result{};
            for (size_t i{}; i < FrameMetricCount; ++i)
                result[i] = values[i].load(std::memory_order_relaxed);
            return result;
        }
    };
    inline FrameCounters frames;

    // Called only by the presentation thread, outside its frame/resource locks.
    struct Reporter {
        uint64_t last{};
        bool Due(uint64_t now) {
            if (last && now - last < 5000000000ULL) return false;
            last = now;
            return true;
        }
    };
}
