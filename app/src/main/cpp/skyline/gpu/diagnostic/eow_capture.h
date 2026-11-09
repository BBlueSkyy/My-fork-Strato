// SPDX-License-Identifier: MPL-2.0
#pragma once

// Zelda: Echoes of Wisdom / Turnip - opt-in, bounded diagnostic only.
// This is NOT a RenderDoc .rdc capture.  It does not alter guest commands or
// request a render-pass break.  The only image copies are explicit snapshots
// queued after an existing render-pass end or at the presentation source.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skyline::gpu::diagnostic {
    // Preserve the numeric bits of Vulkan dispatchable and non-dispatchable
    // handles without assuming the platform typedef is a pointer or integer.
    template <typename Handle>
    std::uint64_t HandleBits(Handle handle) {
        static_assert(sizeof(handle) <= sizeof(std::uint64_t));
        std::uint64_t bits{};
        std::memcpy(&bits, &handle, sizeof(handle));
        return bits;
    }

    class EowCaptureState {
      public:
        static constexpr std::uint32_t FrameCount{4};
        static constexpr std::uint32_t MaxEvents{10000};
        static constexpr std::uint32_t MaxPassSnapshots{12};
        static constexpr std::size_t MaxPassCaptureBytes{64ULL * 1024 * 1024};
        static constexpr std::size_t MaxSingleImageBytes{12ULL * 1024 * 1024};

        std::atomic<bool> active{false};
        std::atomic<bool> passSnapshotsEnabled{false};
        // Approximate VI interval, NOT a GPU command sequence number.
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uintptr_t> currentPresentSource{0};

      private:
        std::mutex mutex;
        std::string directory;
        std::vector<std::string> events;
        std::vector<std::pair<std::uintptr_t, std::size_t>> watchedMappings;
        struct PendingSource {
            std::function<bool()> ready;
            std::function<bool()> write;
        };
        std::vector<PendingSource> pendingSources;
        std::atomic<std::uint32_t> pendingSourceCount{0};
        std::uint32_t sourceRequested{};
        std::uint32_t sourceWritten{};
        std::uint32_t sourceFailed{};
        std::array<std::uint32_t, FrameCount> snapshotsByInterval{};
        std::uint32_t nextSnapshot{};
        std::uint32_t pendingSnapshots{};
        std::uint32_t goodSnapshots{};
        std::uint32_t failedSnapshots{};
        std::size_t snapshotBytes{};
        bool finishedPresentCapture{};

        static bool RangesOverlap(std::uintptr_t a, std::size_t alen,
                                  std::uintptr_t b, std::size_t blen) {
            if (!a || !b || !alen || !blen) return false;
            return a <= b ? b - a < alen : a - b < blen;
        }

        static std::string JsonEscape(const std::string &value) {
            std::string escaped;
            escaped.reserve(value.size() + 8);
            constexpr char hex[] = "0123456789ABCDEF";
            for (unsigned char c : value) {
                if (c == '"' || c == '\\') {
                    escaped.push_back('\\');
                    escaped.push_back(static_cast<char>(c));
                } else if (c < 0x20) {
                    escaped += "\\u00";
                    escaped.push_back(hex[c >> 4]);
                    escaped.push_back(hex[c & 0xf]);
                } else {
                    escaped.push_back(static_cast<char>(c));
                }
            }
            return escaped;
        }

        void MaybeWriteCompletionLocked() {
            if (!finishedPresentCapture || pendingSnapshots) return;
            std::ofstream out(std::filesystem::path(directory) / "pass_readbacks_done.txt");
            if (out)
                out << "requested=" << nextSnapshot << "\n"
                    << "written=" << goodSnapshots << "\n"
                    << "failed=" << failedSnapshots << "\n";
        }

        void MaybeWriteSourceCompletionLocked() {
            if (!finishedPresentCapture || pendingSourceCount.load(std::memory_order_relaxed))
                return;
            std::ofstream out(std::filesystem::path(directory) / "source_readbacks_done.txt");
            if (out)
                out << "requested=" << sourceRequested << "\n"
                    << "written=" << sourceWritten << "\n"
                    << "failed=" << sourceFailed << "\n";
        }

      public:
        void Start(std::string path, bool capturePassSnapshots) {
            std::scoped_lock lock(mutex);
            active.store(false, std::memory_order_release);
            currentPresentSource.store(0, std::memory_order_relaxed);
            directory = std::move(path);
            passSnapshotsEnabled.store(capturePassSnapshots, std::memory_order_relaxed);
            events.clear();
            watchedMappings.clear();
            snapshotsByInterval.fill(0);
            nextSnapshot = pendingSnapshots = goodSnapshots = failedSnapshots = 0;
            snapshotBytes = 0;
            sourceRequested = sourceWritten = sourceFailed = 0;
            finishedPresentCapture = false;
            presentIndex.store(0, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }

        bool CanStartCapture() {
            std::scoped_lock lock(mutex);
            return !active.load(std::memory_order_acquire) &&
                   pendingSnapshots == 0 && pendingSources.empty();
        }

        void SetPresentSource(const void *texture) {
            currentPresentSource.store(reinterpret_cast<std::uintptr_t>(texture),
                                       std::memory_order_release);
        }

        bool IsPresentSource(const void *texture) const {
            return active.load(std::memory_order_acquire) &&
                texture != nullptr &&
                currentPresentSource.load(std::memory_order_acquire) ==
                    reinterpret_cast<std::uintptr_t>(texture);
        }

        void AddPendingSource(std::function<bool()> ready, std::function<bool()> write) {
            std::scoped_lock lock(mutex);
            pendingSources.push_back({std::move(ready), std::move(write)});
            ++sourceRequested;
            pendingSourceCount.store(static_cast<std::uint32_t>(pendingSources.size()),
                                     std::memory_order_release);
        }

        // Called by the existing presentation thread on subsequent frames.
        // Poll(false) never waits for a GPU fence; no extra host synchronization.
        void PollReadySources() {
            if (!pendingSourceCount.load(std::memory_order_acquire)) return;
            std::vector<PendingSource> processing;
            {
                std::scoped_lock lock(mutex);
                processing.swap(pendingSources);
            }
            std::uint32_t good{}, bad{};
            std::vector<PendingSource> remaining;
            for (auto &entry : processing) {
                if (!entry.ready()) {
                    remaining.push_back(std::move(entry));
                } else if (entry.write()) {
                    ++good;
                } else {
                    ++bad;
                }
            }
            {
                std::scoped_lock lock(mutex);
                for (auto &entry : remaining)
                    pendingSources.push_back(std::move(entry));
                sourceWritten += good;
                sourceFailed += bad;
                pendingSourceCount.store(static_cast<std::uint32_t>(pendingSources.size()),
                                         std::memory_order_release);
                MaybeWriteSourceCompletionLocked();
            }
        }

        std::string CaptureDirectory() {
            std::scoped_lock lock(mutex);
            return directory;
        }

        void Record(std::string event) {
            if (!active.load(std::memory_order_acquire)) return;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) || events.size() >= MaxEvents) return;
            events.emplace_back(std::move(event));
        }

        void WatchMapping(std::uintptr_t address, std::size_t length) {
            if (!address || !length || !active.load(std::memory_order_acquire)) return;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed)) return;
            for (auto [existingAddress, existingLength] : watchedMappings)
                if (existingAddress == address && existingLength == length) return;
            if (watchedMappings.size() < 64)
                watchedMappings.emplace_back(address, length);
        }

        bool IsWatchedMapping(std::uintptr_t address, std::size_t length) {
            if (!address || !length || !active.load(std::memory_order_acquire)) return false;
            std::scoped_lock lock(mutex);
            for (auto [watchedAddress, watchedLength] : watchedMappings)
                if (RangesOverlap(address, length, watchedAddress, watchedLength))
                    return true;
            return false;
        }

        std::optional<std::uint32_t> ReserveSnapshot(std::uint32_t interval,
                                                      std::size_t bytes) {
            if (!active.load(std::memory_order_acquire) ||
                !passSnapshotsEnabled.load(std::memory_order_relaxed)) return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed) ||
                interval >= FrameCount || bytes == 0 || bytes > MaxSingleImageBytes ||
                snapshotsByInterval[interval] >= 4 ||
                nextSnapshot >= MaxPassSnapshots ||
                snapshotBytes > MaxPassCaptureBytes - bytes)
                return std::nullopt;
            ++snapshotsByInterval[interval];
            snapshotBytes += bytes;
            ++pendingSnapshots;
            return nextSnapshot++;
        }

        void SnapshotFinished(bool success) {
            std::scoped_lock lock(mutex);
            if (!pendingSnapshots) return;
            --pendingSnapshots;
            if (success) ++goodSnapshots;
            else ++failedSnapshots;
            MaybeWriteCompletionLocked();
        }

        void Finish() {
            active.store(false, std::memory_order_release);
            std::scoped_lock lock(mutex);
            std::ofstream out(std::filesystem::path(directory) / "events.jsonl",
                              std::ios::trunc);
            if (out) {
                for (std::size_t i{}; i < events.size(); ++i)
                    out << "{\"sequence\":" << i
                        << ",\"approx_present\":\"capture_window\",\"event\":\""
                        << JsonEscape(events[i]) << "\"}\n";
            }
            events.clear();
            finishedPresentCapture = true;
            MaybeWriteCompletionLocked();
            MaybeWriteSourceCompletionLocked();
        }
    };

    inline EowCaptureState &EowCapture() {
        static EowCaptureState session;
        return session;
    }
}
