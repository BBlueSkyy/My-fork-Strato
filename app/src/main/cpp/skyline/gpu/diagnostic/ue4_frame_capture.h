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
    class Ue4FrameCapture {
      public:
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint32_t> eventCount{0};
        static constexpr std::uint32_t MaxEvents{12000};
        static constexpr std::uint32_t FrameCount{6};
        // Reserve budget for late UI composition and each of the four intervals.
        static constexpr std::uint32_t PassSnapshotsPerInterval{6};
        static constexpr std::uint32_t MaxPassSnapshots{30};
        static constexpr std::size_t MaxPassCaptureBytes{160ULL * 1024 * 1024};

      private:
        std::mutex mutex;
        std::vector<std::string> events;
        std::string path;
        // One registry shared by all GPU command executors. Individual executor
        // counters collided in the previous capture and overwrote raw files.
        // Phase 3 uses separate quotas from legacy pass-end snapshots.
        // A fixed-size, low-impact scan samples selected HDR draws and
        // the observed HDR->BGRA8 conversion instead of 30 unrelated inputs.
        std::array<std::uint32_t, FrameCount> hdrDrawCountByInterval{};
        std::array<bool, FrameCount> conversionClaimed{};
        std::uint32_t focusSnapshotCount{};
        std::size_t focusBytes{};
        static constexpr std::uint32_t MaxFocusSnapshots{48};
        static constexpr std::size_t MaxFocusBytes{144ULL * 1024 * 1024};
        std::array<std::uint32_t, FrameCount> passCountByInterval{};
        std::uint32_t nextPassIndex{};
        std::size_t passBytes{};
        // Guest backing ranges shared across Maxwell3D, Fermi2D and VI executors.
        // No texture pointers, shader hashes or game IDs are used as persistent keys.
        std::vector<std::pair<std::uintptr_t, std::size_t>> watchedMappings;

      public:
        void Start(std::string captureDirectory) {
            std::scoped_lock lock(mutex);
            path = std::move(captureDirectory);
            events.clear();
            passCountByInterval.fill(0);
            hdrDrawCountByInterval.fill(0);
            conversionClaimed.fill(false);
            focusSnapshotCount = 0;
            focusBytes = 0;
            nextPassIndex = 0;
            passBytes = 0;
            watchedMappings.clear();
            eventCount.store(0, std::memory_order_relaxed);
            presentIndex.store(0, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }

        // One-shot phase 3. All indexes and claims are shared by executor threads.
        // Returns the draw ordinal only at sampling points; the rest do not
        // cause staging allocations or render-pass breaks.
        std::optional<std::uint32_t> ClaimHdrDraw() {
            if (!active.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            const auto frame{presentIndex.load(std::memory_order_relaxed)};
            if (!active.load(std::memory_order_relaxed) || frame >= 2)
                return std::nullopt;
            const auto ordinal{++hdrDrawCountByInterval[frame]};
            if (ordinal == 1 || ordinal == 32 || ordinal == 128 || ordinal == 200)
                return ordinal;
            return std::nullopt;
        }

        bool ClaimConversion() {
            if (!active.load(std::memory_order_acquire))
                return false;
            std::scoped_lock lock(mutex);
            const auto frame{presentIndex.load(std::memory_order_relaxed)};
            if (!active.load(std::memory_order_relaxed) || frame >= FrameCount ||
                conversionClaimed[frame])
                return false;
            conversionClaimed[frame] = true;
            return true;
        }

        std::optional<std::uint32_t> ReserveFocusedImage(std::size_t bytes) {
            if (!bytes || !active.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) ||
                focusSnapshotCount >= MaxFocusSnapshots ||
                bytes > MaxFocusBytes || focusBytes > MaxFocusBytes - bytes)
                return std::nullopt;
            focusBytes += bytes;
            return focusSnapshotCount++;
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
                out << "GPU events collected while 6 frames were being presented.\n"
                    << "present_index_approx is NOT exact GPU/present synchronization.\n"
                    << "Events are capped at 12000 to bound memory and disk usage.\n\n";
                for (const auto &event : events)
                    out << event << '\n';
            }
            events.clear();
        }
    };

    inline Ue4FrameCapture &Ue4Capture() {
        static Ue4FrameCapture session;
        return session;
    }
}