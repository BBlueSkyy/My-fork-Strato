// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only, aggregate GPU performance sampling for DBFZ special attacks.
// The CMake switch defaults ON only in the dedicated diagnostic branch.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <common/utils.h>
#include <logger/logger.h>

namespace skyline::gpu::perf_diag {
    enum class Metric : std::size_t {
        PipelineCreated,
        PipelineSchedule,
        PipelineCompile,
        PipelineFutureGet,
        CommandSlotWait,
        CommandFenceWait,
        CommandRecord,
        PresentGuestFence,
        PresentReconcile,
        PresentSwapchainFence,
        PresentAcquire,
        PresentHostSync,
        PresentCopy,
        PresentQueue,
        Count,
    };

#if defined(STRATO_DBFZ_PERF_DIAGNOSTICS)
    struct Counter {
        std::atomic<std::uint64_t> count{};
        std::atomic<std::uint64_t> totalNs{};
        std::atomic<std::uint64_t> maxNs{};
    };

    struct Sample {
        std::uint64_t count{};
        std::uint64_t totalNs{};
        std::uint64_t maxNs{};
    };

    inline std::array<Counter, static_cast<std::size_t>(Metric::Count)> counters{};

    inline void Record(Metric metric, std::int64_t elapsedNs = 0) noexcept {
        auto &counter{counters[static_cast<std::size_t>(metric)]};
        const auto ns{static_cast<std::uint64_t>(elapsedNs > 0 ? elapsedNs : 0)};
        counter.count.fetch_add(1, std::memory_order_relaxed);
        counter.totalNs.fetch_add(ns, std::memory_order_relaxed);
        auto previous{counter.maxNs.load(std::memory_order_relaxed)};
        while (previous < ns &&
               !counter.maxNs.compare_exchange_weak(previous, ns, std::memory_order_relaxed)) {}
    }

    class Scope {
        Metric metric;
        std::int64_t startNs;

      public:
        explicit Scope(Metric metric) noexcept : metric{metric}, startNs{util::GetTimeNs()} {}
        ~Scope() noexcept { Record(metric, util::GetTimeNs() - startNs); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };

    inline std::array<Sample, static_cast<std::size_t>(Metric::Count)> Drain() noexcept {
        std::array<Sample, static_cast<std::size_t>(Metric::Count)> samples{};
        for (std::size_t i{}; i < samples.size(); i++) {
            samples[i].count = counters[i].count.exchange(0, std::memory_order_relaxed);
            samples[i].totalNs = counters[i].totalNs.exchange(0, std::memory_order_relaxed);
            samples[i].maxNs = counters[i].maxNs.exchange(0, std::memory_order_relaxed);
        }
        return samples;
    }

    inline double Ms(std::uint64_t nanoseconds) noexcept {
        return static_cast<double>(nanoseconds) / 1'000'000.0;
    }

    // PresentationThread is the sole caller. The metrics are collected on other threads
    // without locks or per-frame logging. Each window reports aggregate wall-time,
    // not GPU execution time; sums from concurrent threads can overlap.
    inline void OnPresent(std::int64_t presentDeltaNs, int displayedFps) {
        struct Window {
            std::int64_t startNs{};
            std::int64_t previousWallNs{};
            std::uint64_t frames{};
            std::uint64_t sumPresentNs{};
            std::uint64_t maxPresentNs{};
            std::uint64_t sumWallNs{};
            std::uint64_t maxWallNs{};
            std::uint64_t slow22{};
            std::uint64_t slow33{};
        };
        static Window window{};
        const auto now{util::GetTimeNs()};
        if (!window.startNs)
            window.startNs = now;
        if (presentDeltaNs > 0) {
            const auto delta{static_cast<std::uint64_t>(presentDeltaNs)};
            window.frames++;
            window.sumPresentNs += delta;
            if (delta > window.maxPresentNs)
                window.maxPresentNs = delta;
            if (delta >= 22'000'000)
                window.slow22++;
            if (delta >= 33'000'000)
                window.slow33++;
        }
        if (window.previousWallNs && now > window.previousWallNs) {
            const auto delta{static_cast<std::uint64_t>(now - window.previousWallNs)};
            window.sumWallNs += delta;
            if (delta > window.maxWallNs)
                window.maxWallNs = delta;
        }
        window.previousWallNs = now;
        if (now - window.startNs < 2'000'000'000)
            return;

        const auto samples{Drain()};
        const auto get{[&](Metric metric) -> const Sample & {
            return samples[static_cast<std::size_t>(metric)];
        }};
        const auto &created{get(Metric::PipelineCreated)};
        const auto &compile{get(Metric::PipelineCompile)};
        const auto &future{get(Metric::PipelineFutureGet)};
        const auto &slot{get(Metric::CommandSlotWait)};
        const auto &fence{get(Metric::CommandFenceWait)};
        const auto &record{get(Metric::CommandRecord)};
        LOGI("[DBFZ-PERF] frames={} fps={} present_avg/max_ms={:.2f}/{:.2f} wall_avg/max_ms={:.2f}/{:.2f} slow22/33={}/{} pipelines_new={} compile_count={} compile_sum/max_ms={:.2f}/{:.2f} future_get_count={} future_get_sum/max_ms={:.2f}/{:.2f}",
             window.frames, displayedFps,
             window.frames ? Ms(window.sumPresentNs) / window.frames : 0.0, Ms(window.maxPresentNs),
             window.frames ? Ms(window.sumWallNs) / window.frames : 0.0, Ms(window.maxWallNs),
             window.slow22, window.slow33, created.count, compile.count,
             Ms(compile.totalNs), Ms(compile.maxNs), future.count, Ms(future.totalNs), Ms(future.maxNs));
        LOGI("[DBFZ-PERF] CPU_WINDOW_MS (overlapping threads) pipeline_schedule={:.2f} cmd_slot_wait={:.2f}({}) cmd_fence_wait={:.2f}({}) cmd_record={:.2f}({}) present_guest_fence={:.2f} present_reconcile={:.2f} present_swapchain_fence={:.2f} present_acquire={:.2f} present_host_sync={:.2f} present_copy={:.2f} present_queue={:.2f}",
             Ms(get(Metric::PipelineSchedule).totalNs),
             Ms(slot.totalNs), slot.count, Ms(fence.totalNs), fence.count,
             Ms(record.totalNs), record.count,
             Ms(get(Metric::PresentGuestFence).totalNs),
             Ms(get(Metric::PresentReconcile).totalNs),
             Ms(get(Metric::PresentSwapchainFence).totalNs),
             Ms(get(Metric::PresentAcquire).totalNs),
             Ms(get(Metric::PresentHostSync).totalNs),
             Ms(get(Metric::PresentCopy).totalNs),
             Ms(get(Metric::PresentQueue).totalNs));
        window = Window{};
        window.startNs = now;
        window.previousWallNs = now;
    }
#else
    inline void Record(Metric, std::int64_t = 0) noexcept {}
    class Scope {
      public:
        explicit Scope(Metric) noexcept {}
    };
    inline void OnPresent(std::int64_t, int) {}
#endif
}
