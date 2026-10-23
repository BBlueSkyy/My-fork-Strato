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
    class Ue4UiCaptureState {
      public:
        std::atomic<bool> active{false};
        std::atomic<bool> drawStepMode{false};
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint32_t> eventCount{0};
        static constexpr std::uint32_t MaxEvents{10000};
        static constexpr std::uint32_t FrameCount{4};
        // Reserve budget for late UI composition and each of the four intervals.
        static constexpr std::uint32_t PassSnapshotsPerInterval{5};
        static constexpr std::uint32_t MaxPassSnapshots{20};
        static constexpr std::size_t MaxPassCaptureBytes{96ULL * 1024 * 1024};

      private:
        std::mutex mutex;
        std::vector<std::string> events;
        std::string path;
        // One registry shared by all GPU command executors. Individual executor
        // counters collided in the previous capture and overwrote raw files.
        std::array<std::uint32_t, FrameCount> passCountByInterval{};
        std::uint32_t nextPassIndex{};
        std::size_t passBytes{};
        std::uint32_t pendingPassSnapshots{};
        std::uint32_t successfulPassSnapshots{};
        std::uint32_t failedPassSnapshots{};
        bool presentationSamplesFinished{};
        // Only first observed UI quad sequence per physical presentation image.
        // Diagnostic ordinals are per Texture identity and never used as guest frame IDs.
        std::vector<std::pair<std::uintptr_t, std::uint32_t>> uiDrawCounts;

        // Must be called while mutex is held. This marker is different from
        // capture_complete.txt: it only appears once every reserved pass
        // readback callback has finished writing (or reported failure).
        void MaybeWritePassCompletionLocked() {
            if (!presentationSamplesFinished || pendingPassSnapshots)
                return;
            std::ofstream out(std::filesystem::path(path) / "pass_readbacks_done.txt",
                              std::ios::trunc);
            if (out)
                out << "readback_callbacks_finished=true\n"
                    << "requested=" << nextPassIndex << "\n"
                    << "written=" << successfulPassSnapshots << "\n"
                    << "failed=" << failedPassSnapshots << "\n";
        }
        // Guest backing ranges shared across Maxwell3D, Fermi2D and VI executors.
        // No texture pointers, shader hashes or game IDs are used as persistent keys.
        std::vector<std::pair<std::uintptr_t, std::size_t>> watchedMappings;

      public:
        void Start(std::string captureDirectory, bool enableDrawSteps = false) {
            std::scoped_lock lock(mutex);
            path = std::move(captureDirectory);
            events.clear();
            passCountByInterval.fill(0);
            nextPassIndex = 0;
            passBytes = 0;
            pendingPassSnapshots = 0;
            successfulPassSnapshots = 0;
            failedPassSnapshots = 0;
            presentationSamplesFinished = false;
            uiDrawCounts.clear();
            drawStepMode.store(enableDrawSteps, std::memory_order_relaxed);
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
            ++pendingPassSnapshots;
            return nextPassIndex++;
        }

        std::optional<std::uint32_t> ClaimUiDrawStep(std::uintptr_t textureIdentity) {
            if (!textureIdentity || !active.load(std::memory_order_acquire) ||
                !drawStepMode.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed))
                return std::nullopt;
            for (auto &[identity, ordinal] : uiDrawCounts) {
                if (identity != textureIdentity)
                    continue;
                if (ordinal >= 48)
                    return std::nullopt;
                ++ordinal;
                if (ordinal == 1 || ordinal == 8 || ordinal == 16 ||
                    ordinal == 32 || ordinal == 47)
                    return ordinal;
                return std::nullopt;
            }
            if (uiDrawCounts.size() < 4) {
                uiDrawCounts.emplace_back(textureIdentity, 1);
                return 1;
            }
            return std::nullopt;
        }

        void PassSnapshotFinished(bool success) {
            std::scoped_lock lock(mutex);
            if (!pendingPassSnapshots)
                return;
            --pendingPassSnapshots;
            if (success)
                ++successfulPassSnapshots;
            else
                ++failedPassSnapshots;
            MaybeWritePassCompletionLocked();
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
                out << "GPU events collected while 4 frames were being presented.\n"
                    << "present_index_approx is NOT exact GPU/present synchronization.\n"
                    << "Events are capped at 10000 to bound memory and disk usage.\n\n";
                for (const auto &event : events)
                    out << event << '\n';
            }
            events.clear();
            presentationSamplesFinished = true;
            MaybeWritePassCompletionLocked();
        }
    };

    inline Ue4UiCaptureState &UiSurfaceCapture() {
        static Ue4UiCaptureState session;
        return session;
    }
}
