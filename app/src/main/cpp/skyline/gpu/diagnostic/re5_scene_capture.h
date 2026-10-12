// SPDX-License-Identifier: MPL-2.0
#pragma once
// One-shot diagnostic only. This is a small, bounded trace, not the RenderDoc API.
// No guest data or guest GPU command is modified.
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

namespace skyline::gpu::diagnostic {
    class SceneFrameCapture {
      public:
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint32_t> eventCount{0};
        static constexpr std::uint32_t MaxEvents{2400};
        static constexpr std::uint32_t FrameCount{4};
        // At most six post-pass color snapshots in each approximate presentation interval.
        static constexpr std::uint32_t PassSnapshotsPerInterval{6};
        static constexpr std::uint32_t MaxPassSnapshots{24};
        static constexpr std::size_t MaxPassCaptureBytes{100ULL * 1024 * 1024};
        static constexpr std::uint32_t ScalarProbesPerInterval{32};
        struct ScalarTicket {
            std::uint32_t index;
            std::string directory;
        };

      private:
        std::mutex mutex;
        std::vector<std::string> events;
        std::string path;
        // One registry shared by all GPU command executors. Individual executor
        // counters collided in the previous capture and overwrote raw files.
        std::array<std::uint32_t, FrameCount> passCountByInterval{};
        std::uint32_t nextPassIndex{};
        std::size_t passBytes{};
        std::array<std::atomic<std::uint32_t>, FrameCount> priorityDrawsByInterval{};
        std::array<std::atomic<std::uint32_t>, FrameCount> otherDrawsByInterval{};
        std::array<std::uint32_t, FrameCount> scalarCountByInterval{};
        std::array<std::array<std::uint32_t, 2>, FrameCount> scalarDrawsByInterval{};
        std::uint32_t nextScalarIndex{};

      public:
        void Start(std::string captureDirectory) {
            std::scoped_lock lock(mutex);
            path = std::move(captureDirectory);
            events.clear();
            passCountByInterval.fill(0);
            nextPassIndex = 0;
            passBytes = 0;
            scalarCountByInterval.fill(0);
            scalarDrawsByInterval = {};
            nextScalarIndex = 0;
            for (auto &counter : priorityDrawsByInterval)
                counter.store(0, std::memory_order_relaxed);
            for (auto &counter : otherDrawsByInterval)
                counter.store(0, std::memory_order_relaxed);
            eventCount.store(0, std::memory_order_relaxed);
            presentIndex.store(0, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }

        // Allocates a unique, globally capped snapshot ID across all executors.
        // Called before allocating staging buffers or writing file names.
        std::optional<std::uint32_t> ReservePassSnapshot(std::uint32_t interval, std::size_t bytes) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) ||
                interval >= FrameCount || bytes == 0 ||
                passCountByInterval[interval] >= PassSnapshotsPerInterval ||
                nextPassIndex >= MaxPassSnapshots ||
                passBytes > MaxPassCaptureBytes ||
                bytes > MaxPassCaptureBytes - passBytes)
                return std::nullopt;

            ++passCountByInterval[interval];
            passBytes += bytes;
            return nextPassIndex++;
        }

        // This samples *real* scene draws; no image copies or guest state changes.
        // Per-approximate-frame counters prevent the first busy frame from using
        // the entire bounded event budget of a four-frame capture.
        std::optional<std::uint32_t> SampleSceneDraw(bool priorityShader) {
            if (!active.load(std::memory_order_acquire))
                return std::nullopt;
            const auto interval{presentIndex.load(std::memory_order_acquire)};
            if (interval >= FrameCount)
                return std::nullopt;
            auto &counter{priorityShader ? priorityDrawsByInterval[interval] : otherDrawsByInterval[interval]};
            const auto ordinal{counter.fetch_add(1, std::memory_order_relaxed)};
            if (priorityShader ? (ordinal < 8 || ordinal % 32 == 0)
                               : (ordinal < 4 || ordinal % 128 == 0))
                return ordinal;
            return std::nullopt;
        }

        std::string CaptureDirectory() {
            std::scoped_lock lock(mutex);
            return path;
        }

        std::optional<ScalarTicket> ReserveScalarProbe(std::uint32_t interval) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || interval >= FrameCount ||
                scalarCountByInterval[interval] >= ScalarProbesPerInterval)
                return std::nullopt;
            ++scalarCountByInterval[interval];
            return ScalarTicket{nextScalarIndex++, path};
        }

        bool SampleScalarDraw(std::uint32_t interval, std::uint32_t target) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || interval >= FrameCount || target >= 2)
                return false;
            const auto ordinal{scalarDrawsByInterval[interval][target]++};
            return ordinal < 8 || ordinal % 32 == 0;
        }

        void Record(std::string event) {
            if (!active.load(std::memory_order_acquire) ||
                eventCount.load(std::memory_order_relaxed) >= MaxEvents)
                return;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) || events.size() >= MaxEvents)
                return;
            events.emplace_back("present_index_approx=" +
                std::to_string(presentIndex.load(std::memory_order_relaxed)) + " " +
                std::move(event));
            eventCount.store(static_cast<std::uint32_t>(events.size()), std::memory_order_relaxed);
        }

        void Finish() {
            active.store(false, std::memory_order_release);
            std::scoped_lock lock(mutex);
            std::ofstream out(std::filesystem::path(path) / "gpu_events.txt", std::ios::trunc);
            if (out) {
                out << "GPU events collected while 4 frames were being presented.\n"
                    << "present_index_approx is NOT exact GPU/present synchronization.\n"
                    << "Events are filtered for 1280x720 scene draws and sampled to preserve later frames.\n\n";
                for (const auto &event : events)
                    out << event << '\n';
            }
            events.clear();
        }
    };

    inline SceneFrameCapture &Re5SceneCapture() {
        static SceneFrameCapture session;
        return session;
    }
}
