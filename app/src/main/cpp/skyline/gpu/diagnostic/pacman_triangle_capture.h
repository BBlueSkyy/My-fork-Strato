// SPDX-License-Identifier: MPL-2.0
#pragma once
// Bounded, opt-in PAC-MAN capture. Not a full RenderDoc .rdc.
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skyline::gpu::diagnostic {
    class PacmanTriangleCapture {
        static constexpr std::uint32_t MaxEvents{12000};
        static constexpr std::uint32_t MaxPresentedFrames{6};
        static constexpr std::uint32_t MaxSnapshots{14};
        static constexpr std::size_t MaxSnapshotBytes{64ULL * 1024 * 1024};

        mutable std::mutex mutex;
        std::filesystem::path destination;
        std::vector<std::string> events;
        std::atomic<bool> active{false};
        std::atomic<bool> requestRenderDoc{false};
        std::atomic<bool> snapshotsEnabled{false};
        std::atomic<std::uint32_t> presentedFrames{0};
        std::uint32_t droppedEvents{};
        std::array<std::uint32_t, MaxPresentedFrames> sceneOrdinals{};
        std::size_t reservedBytes{};
        std::uint32_t reservedSnapshots{};
        std::uint32_t pendingSnapshots{};
        std::uint32_t writtenSnapshots{};
        std::uint32_t failedSnapshots{};
        bool presentationsFinished{};

        void MaybeFinishSnapshotsLocked() {
            if (!presentationsFinished || pendingSnapshots || !snapshotsEnabled.load())
                return;
            std::ofstream out{destination / "snapshot_readbacks_done.txt", std::ios::trunc};
            if (out)
                out << "requested=" << reservedSnapshots << '\n'
                    << "written=" << writtenSnapshots << '\n'
                    << "failed=" << failedSnapshots << '\n'
                    << "bytes_reserved=" << reservedBytes << '\n';
        }
      public:
        bool Active() const { return active.load(std::memory_order_acquire); }
        bool SnapshotsEnabled() const {
            return Active() && snapshotsEnabled.load(std::memory_order_acquire);
        }
        std::uint32_t PresentedFrames() const {
            return presentedFrames.load(std::memory_order_relaxed);
        }
        std::filesystem::path CaptureDirectory() const {
            std::lock_guard lock{mutex};
            return destination;
        }
        void Start(std::filesystem::path folder, bool enableSnapshots = false) {
            std::lock_guard lock{mutex};
            destination = std::move(folder);
            events.clear();
            events.reserve(MaxEvents);
            droppedEvents = 0;
            sceneOrdinals.fill(0);
            reservedBytes = 0;
            reservedSnapshots = pendingSnapshots = writtenSnapshots = failedSnapshots = 0;
            presentationsFinished = false;
            presentedFrames.store(0, std::memory_order_relaxed);
            snapshotsEnabled.store(enableSnapshots, std::memory_order_release);
            requestRenderDoc.store(true, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }
        void Record(std::string event) {
            if (!Active()) return;
            std::lock_guard lock{mutex};
            if (!Active()) return;
            if (events.size() < MaxEvents)
                events.emplace_back(std::move(event));
            else ++droppedEvents;
        }
        bool TakeRenderDocRequest() {
            return Active() && requestRenderDoc.exchange(false, std::memory_order_acq_rel);
        }
        // First two complete scene intervals; each draw can claim one ordinal.
        std::optional<std::uint32_t> NextSceneOrdinal() {
            if (!SnapshotsEnabled()) return {};
            const auto interval{PresentedFrames()};
            if (interval != 1 && interval != 2) return {};
            std::lock_guard lock{mutex};
            if (!Active()) return {};
            return ++sceneOrdinals[interval];
        }
        std::optional<std::uint32_t> ReserveSnapshot(std::size_t bytes) {
            if (!SnapshotsEnabled() || !bytes || bytes > 8ULL * 1024 * 1024)
                return {};
            std::lock_guard lock{mutex};
            if (!Active() || reservedSnapshots >= MaxSnapshots ||
                reservedBytes > MaxSnapshotBytes - bytes)
                return {};
            reservedBytes += bytes;
            ++pendingSnapshots;
            return reservedSnapshots++;
        }
        void SnapshotFinished(bool success) {
            std::lock_guard lock{mutex};
            if (pendingSnapshots) --pendingSnapshots;
            if (success) ++writtenSnapshots;
            else ++failedSnapshots;
            MaybeFinishSnapshotsLocked();
        }
        // Present-index intervals are approximate and not guest GPU frame boundaries.
        void CompletePresentation() {
            if (!Active()) return;
            if (presentedFrames.fetch_add(1, std::memory_order_acq_rel) + 1 <
                MaxPresentedFrames) return;
            std::lock_guard lock{mutex};
            if (!active.exchange(false, std::memory_order_acq_rel)) return;
            presentationsFinished = true;
            std::ofstream trace{destination / "gpu_events.txt", std::ios::trunc};
            if (trace) {
                trace << "PAC-MAN WORLD Re-PAC mini diagnostic, NOT an RDC file\n"
                      << "present intervals approximate; GPU submissions are asynchronous\n";
                for (const auto &line : events) trace << line << '\n';
                trace << "dropped_events=" << droppedEvents << '\n';
            }
            std::ofstream complete{destination / "capture_complete.txt", std::ios::trunc};
            if (complete)
                complete << "recorded_presentations=" << MaxPresentedFrames << '\n'
                         << "events_written=" << events.size() << '\n'
                         << "dropped_events=" << droppedEvents << '\n'
                         << "snapshot_mode=" << snapshotsEnabled.load() << '\n';
            MaybeFinishSnapshotsLocked();
        }
    };

    inline PacmanTriangleCapture &PacmanCapture() {
        static PacmanTriangleCapture capture;
        return capture;
    }
}
