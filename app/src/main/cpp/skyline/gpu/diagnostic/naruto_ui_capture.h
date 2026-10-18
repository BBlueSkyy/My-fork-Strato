// SPDX-License-Identifier: MPL-2.0
#pragma once
// One-shot diagnostic only. This is a small, bounded trace, not the RenderDoc API.
// No guest data or guest GPU command is modified.
#include <algorithm>
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
    class UiFrameCapture {
      public:
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint32_t> eventCount{0};
        static constexpr std::uint32_t MaxEvents{2400};
        static constexpr std::uint32_t FrameCount{4};
        // Reserve budget for late UI composition and each of the four intervals.
        static constexpr std::uint32_t PassSnapshotsPerInterval{8};
        static constexpr std::uint32_t MaxPassSnapshots{32};
        static constexpr std::uint32_t AtlasSnapshotsPerInterval{3};
        static constexpr std::size_t MaxPassCaptureBytes{160ULL * 1024 * 1024};

      private:
        std::mutex mutex;
        std::vector<std::string> events;
        std::string path;
        // One registry shared by all GPU command executors. Individual executor
        // counters collided in the previous capture and overwrote raw files.
        std::array<std::uint32_t, FrameCount> passCountByInterval{};
        std::uint32_t nextPassIndex{};
        std::size_t passBytes{};
        std::array<std::uint32_t, FrameCount> atlasCountByInterval{};
        std::array<std::uint32_t, FrameCount> uiDrawCountByInterval{};
        std::array<bool, FrameCount> preUiDrawClaimed{};
        std::vector<std::uintptr_t> capturedAtlasIdentities;
        // Guest backing ranges shared across Maxwell3D, Fermi2D and VI executors.
        // No texture pointers, shader hashes or game IDs are used as persistent keys.
        std::vector<std::pair<std::uintptr_t, std::size_t>> watchedMappings;

      public:
        void Start(std::string captureDirectory) {
            std::scoped_lock lock(mutex);
            path = std::move(captureDirectory);
            events.clear();
            passCountByInterval.fill(0);
            atlasCountByInterval.fill(0);
            uiDrawCountByInterval.fill(0);
            preUiDrawClaimed.fill(false);
            capturedAtlasIdentities.clear();
            nextPassIndex = 0;
            passBytes = 0;
            watchedMappings.clear();
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

        // One-shot diagnostic: a blended draw sampling compressed assets may
        // produce a sprite/UI layer. Follow its backing across executors.
        void WatchMapping(std::uintptr_t address, std::size_t bytes) {
            if (!address || !bytes || !active.load(std::memory_order_acquire))
                return;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed))
                return;
            for (const auto &entry : watchedMappings)
                if (entry.first == address && entry.second == bytes)
                    return;
            if (watchedMappings.size() < 32)
                watchedMappings.emplace_back(address, bytes);
        }

        bool IsWatchedMapping(std::uintptr_t address, std::size_t bytes) {
            if (!address || !bytes || !active.load(std::memory_order_acquire))
                return false;
            std::scoped_lock lock(mutex);
            for (const auto &[start, length] : watchedMappings) {
                if (address >= start ? address - start < length : start - address < bytes)
                    return true;
            }
            return false;
        }

        // First *matching* sprite draw in each approximate presentation interval.
        // Claim before recording the draw, so a true pre-draw image can be queued.
        bool ClaimPreUiDraw(std::uint32_t interval) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || interval >= FrameCount ||
                preUiDrawClaimed[interval])
                return false;
            preUiDrawClaimed[interval] = true;
            return true;
        }

        // A tiny number of intermediate UI draw boundaries is sampled. This
        // intentionally changes render-pass grouping only while capture is armed,
        // never during regular gameplay.
        std::optional<std::uint32_t> ProbeUiDraw(std::uint32_t interval) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || interval >= FrameCount)
                return std::nullopt;
            const auto ordinal{++uiDrawCountByInterval[interval]};
            if (ordinal == 1 || ordinal == 3 || ordinal == 6)
                return ordinal;
            return std::nullopt;
        }

        // Suppress redundant atlases across executors and presentation intervals.
        std::optional<std::uint32_t> ReserveAtlasSnapshot(std::uint32_t interval,
                                                            std::uintptr_t imageIdentity,
                                                            std::size_t bytes) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || !imageIdentity ||
                interval >= FrameCount || bytes == 0 ||
                atlasCountByInterval[interval] >= AtlasSnapshotsPerInterval ||
                std::find(capturedAtlasIdentities.begin(), capturedAtlasIdentities.end(), imageIdentity) != capturedAtlasIdentities.end() ||
                passCountByInterval[interval] >= PassSnapshotsPerInterval ||
                nextPassIndex >= MaxPassSnapshots ||
                passBytes > MaxPassCaptureBytes || bytes > MaxPassCaptureBytes - passBytes)
                return std::nullopt;

            ++atlasCountByInterval[interval];
            ++passCountByInterval[interval];
            passBytes += bytes;
            capturedAtlasIdentities.push_back(imageIdentity);
            return nextPassIndex++;
        }

        std::string CaptureDirectory() {
            std::scoped_lock lock(mutex);
            return path;
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
                    << "Events are capped at 2400 to bound memory and disk usage.\n\n";
                for (const auto &event : events)
                    out << event << '\n';
            }
            events.clear();
        }
    };

    inline UiFrameCapture &NarutoUiCapture() {
        static UiFrameCapture session;
        return session;
    }
}
