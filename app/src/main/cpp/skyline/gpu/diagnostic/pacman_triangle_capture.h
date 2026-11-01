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
        static constexpr std::uint32_t MaxScanIntervals{60};
        static constexpr std::uint32_t MaxScanReadbacks{60};
        static constexpr std::uint32_t MaxSnapshots{8};
        static constexpr std::size_t MaxSnapshotBytes{64ULL * 1024 * 1024};

        mutable std::mutex mutex;
        std::filesystem::path destination;
        std::vector<std::string> events;
        std::atomic<bool> active{false};
        std::atomic<bool> requestRenderDoc{false};
        std::atomic<bool> snapshotsEnabled{false};
        std::atomic<bool> scanMode{false};
        // 0=normal capture, 1=renderpass split, 2=split + Vulkan barrier.
        std::atomic<std::uint32_t> executorExperimentMode{0};
        std::atomic<std::uint64_t> executorExperimentSteps{0};
        std::atomic<std::uint32_t> presentedFrames{0};
        std::uint32_t droppedEvents{};
        std::array<std::uint32_t, MaxScanIntervals + 1> sceneOrdinals{};
        std::uint32_t scanRequested{}, scanPending{}, scanCompleted{}, scanKept{};
        std::uint32_t scanAnomalous{}, scanFailed{};
        bool scanBaselineCaptured{};
        std::uint64_t generation{};
        std::size_t reservedBytes{};
        std::uint32_t reservedSnapshots{};
        std::uint32_t pendingSnapshots{};
        std::uint32_t writtenSnapshots{};
        std::uint32_t failedSnapshots{};
        bool presentationsFinished{};

        void MaybeFinishSnapshotsLocked() {
            if (!presentationsFinished || !snapshotsEnabled.load()) return;
            if (scanMode.load()) {
                if (scanPending) return;
                std::ofstream out{destination / "hdr_scan_done.txt", std::ios::trunc};
                if (out)
                    out << "requested=" << scanRequested << '\n'
                        << "completed=" << scanCompleted << '\n'
                        << "anomaly_matches=" << scanAnomalous << '\n'
                        << "images_saved=" << scanKept << '\n'
                        << "failed=" << scanFailed << '\n';
                return;
            }
            if (pendingSnapshots) return;
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
        bool ScanEnabled() const {
            return Active() && scanMode.load(std::memory_order_acquire);
        }
        std::uint32_t ExecutorExperimentMode() const {
            return Active() ? executorExperimentMode.load(std::memory_order_relaxed) : 0;
        }
        void ExecutorExperimentStep() {
            executorExperimentSteps.fetch_add(1, std::memory_order_relaxed);
        }
        std::uint32_t PresentedFrames() const {
            return presentedFrames.load(std::memory_order_relaxed);
        }
        std::filesystem::path CaptureDirectory() const {
            std::lock_guard lock{mutex};
            return destination;
        }
        void Start(std::filesystem::path folder, bool enableSnapshots = false,
                   bool enableScan = false, std::uint32_t experimentMode = 0) {
            std::lock_guard lock{mutex};
            destination = std::move(folder);
            events.clear();
            events.reserve(MaxEvents);
            droppedEvents = 0;
            sceneOrdinals.fill(0);
            executorExperimentSteps.store(0, std::memory_order_relaxed);
            ++generation;
            scanRequested = scanPending = scanCompleted = scanKept = 0;
            scanAnomalous = scanFailed = 0;
            scanBaselineCaptured = false;
            reservedBytes = 0;
            reservedSnapshots = pendingSnapshots = writtenSnapshots = failedSnapshots = 0;
            presentationsFinished = false;
            presentedFrames.store(0, std::memory_order_relaxed);
            snapshotsEnabled.store((enableSnapshots || enableScan) && !experimentMode, std::memory_order_release);
            scanMode.store(enableScan && !experimentMode, std::memory_order_release);
            executorExperimentMode.store(experimentMode, std::memory_order_release);
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
        // Host-present intervals are approximate. A scan checks the last
        // eligible scene draw for up to 60 intervals; fixed capture uses one.
        std::optional<std::uint32_t> NextSceneOrdinal() {
            if (!SnapshotsEnabled()) return {};
            const auto interval{PresentedFrames()};
            if (ScanEnabled()) {
                if (interval < 1 || interval > MaxScanIntervals) return {};
            } else if (interval != 1) return {};
            std::lock_guard lock{mutex};
            if (!Active()) return {};
            return ++sceneOrdinals[interval];
        }
        // Scan staging remains GPU-owned until its fence completes. Only the
        // first clean baseline and up to four abnormal images are persisted.
        std::optional<std::pair<std::uint32_t, std::uint64_t>> ReserveScan(std::size_t bytes) {
            if (!ScanEnabled() || bytes != 1280ULL * 720 * 4) return {};
            std::lock_guard lock{mutex};
            // Cap scan staging to 4 in-flight images (~14.1 MiB), not 60.
            if (!Active() || scanRequested >= MaxScanReadbacks || scanPending >= 4) return {};
            ++scanPending;
            return std::make_pair(scanRequested++, generation);
        }
        bool KeepScanImage(std::uint64_t epoch, std::uint32_t saturatedPixels) {
            std::lock_guard lock{mutex};
            if (epoch != generation) return false;
            const bool abnormal{saturatedPixels >= 512};
            if (abnormal) ++scanAnomalous;
            // First completed scan is a baseline. Retain at most four
            // anomalous examples, under a strict 5-image output limit.
            const bool baseline{!scanBaselineCaptured};
            if (baseline) scanBaselineCaptured = true;
            const bool save{(baseline || abnormal) && scanKept < 5};
            if (save) ++scanKept;
            return save;
        }
        void ScanFinished(std::uint64_t epoch, std::uint32_t sample,
                          std::uint32_t saturatedPixels, bool success) {
            std::lock_guard lock{mutex};
            if (epoch != generation) return;
            if (scanPending) --scanPending;
            ++scanCompleted;
            if (!success) ++scanFailed;
            std::ofstream csv{destination / "hdr_scan.csv", std::ios::app};
            if (csv) csv << sample << ',' << saturatedPixels << ',' << success << '\n';
            MaybeFinishSnapshotsLocked();
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
                ((scanMode.load() || executorExperimentMode.load()) ? MaxScanIntervals : MaxPresentedFrames)) return;
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
                complete << "recorded_presentations=" << ((scanMode.load() || executorExperimentMode.load()) ? MaxScanIntervals : MaxPresentedFrames) << '\n'
                         << "events_written=" << events.size() << '\n'
                         << "dropped_events=" << droppedEvents << '\n'
                         << "snapshot_mode=" << snapshotsEnabled.load() << '\n'
                         << "hdr_scan_mode=" << scanMode.load() << '\n'
                         << "executor_experiment_mode=" << executorExperimentMode.load() << '\n'
                         << "executor_experiment_draws=" << executorExperimentSteps.load() << '\n';
            MaybeFinishSnapshotsLocked();
        }
    };

    inline PacmanTriangleCapture &PacmanCapture() {
        static PacmanTriangleCapture capture;
        return capture;
    }
}
