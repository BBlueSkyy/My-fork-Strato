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
        static constexpr std::uint32_t FrameCount{6};
        static constexpr std::uint32_t MaxEvents{10000};
        static constexpr std::uint32_t MaxPassSnapshots{12};
        static constexpr std::size_t MaxPassCaptureBytes{64ULL * 1024 * 1024};
        static constexpr std::size_t MaxSingleImageBytes{12ULL * 1024 * 1024};

        std::atomic<bool> active{false};
        std::atomic<bool> passSnapshotsEnabled{false};
        // Separate optional stage: reserve the snapshot budget for the
        // largest live input to postprocessing, on subsequent world passes.
        std::atomic<bool> sceneProducerOnly{false};
        std::atomic<std::uintptr_t> primarySceneTexture{0};
        // Marker-controlled CPU-only scene activity gate. It remains active
        // before a capture starts so splash/menu VI frames do not consume
        // the short pass-readback window.
        std::atomic<bool> sceneGateArmed{false};
        std::atomic<std::uint32_t> sceneGeometryDraws{0};
        static constexpr std::uint32_t MinSceneGeometryDraws{12};
        // Approximate VI interval, NOT a GPU command sequence number.
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uintptr_t> currentPresentSource{0};
        // Discovered from an actual Fermi blit whose destination overlaps VI.
        // Used only to select existing render-pass-end readbacks on later frames.
        std::atomic<std::uintptr_t> finalFermiInput{0};

      private:
        std::mutex mutex;
        std::string directory;
        std::vector<std::string> events;
        std::vector<std::pair<std::uintptr_t, std::size_t>> watchedMappings;
        // Actual sampled views of the draw writing the discovered final HDR target.
        // Pointer values are used solely as capture-local identities.
        std::array<std::uintptr_t, 8> compositionInputs{};
        // Sampled views used by the last producer of a composition input.
        // They are discovered on subsequent natural frames after the VI->HDR->
        // composition chain has been resolved from live Vulkan resources.
        std::array<std::uintptr_t, 8> upstreamInputs{};
        std::size_t largestUpstreamBytes{};
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
        bool ArmSceneGate() {
            if (sceneGateArmed.exchange(true, std::memory_order_acq_rel))
                return false;
            sceneGeometryDraws.store(0, std::memory_order_relaxed);
            return true;
        }

        // Very cheap, CPU-only trigger while waiting for scene activity;
        // no Vulkan calls, no GPU readback and no game-specific shader IDs.
        void NoteSceneGeometryDraw(bool indexed, std::uint32_t count,
                                   std::uint32_t instances) {
            if (!sceneGateArmed.load(std::memory_order_relaxed) ||
                !indexed || count < 96 || !instances)
                return;
            auto observed{sceneGeometryDraws.load(std::memory_order_relaxed)};
            if (observed < MinSceneGeometryDraws)
                sceneGeometryDraws.fetch_add(1, std::memory_order_relaxed);
        }

        bool SceneGateReady() const {
            return sceneGateArmed.load(std::memory_order_acquire) &&
                sceneGeometryDraws.load(std::memory_order_relaxed) >=
                    MinSceneGeometryDraws;
        }

        std::uint32_t SceneGateDrawCount() const {
            return sceneGeometryDraws.load(std::memory_order_relaxed);
        }

        void DisarmSceneGate() {
            sceneGateArmed.store(false, std::memory_order_release);
            sceneGeometryDraws.store(0, std::memory_order_relaxed);
        }

        void Start(std::string path, bool capturePassSnapshots,
                   bool sceneProducerCapture = false) {
            std::scoped_lock lock(mutex);
            active.store(false, std::memory_order_release);
            DisarmSceneGate();
            currentPresentSource.store(0, std::memory_order_relaxed);
            finalFermiInput.store(0, std::memory_order_relaxed);
            primarySceneTexture.store(0, std::memory_order_relaxed);
            sceneProducerOnly.store(sceneProducerCapture && capturePassSnapshots,
                                    std::memory_order_relaxed);
            directory = std::move(path);
            passSnapshotsEnabled.store(capturePassSnapshots, std::memory_order_relaxed);
            events.clear();
            watchedMappings.clear();
            compositionInputs.fill(0);
            upstreamInputs.fill(0);
            largestUpstreamBytes = 0;
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

        bool ObserveFinalFermiInput(const void *texture) {
            if (!texture || !active.load(std::memory_order_acquire) ||
                !passSnapshotsEnabled.load(std::memory_order_relaxed))
                return false;
            const auto id{reinterpret_cast<std::uintptr_t>(texture)};
            // The same source may appear in several VI presentations; do not
            // record or restart capture for each one.
            const auto last{finalFermiInput.exchange(id, std::memory_order_acq_rel)};
            return last != id;
        }

        bool IsFinalFermiInput(const void *texture) const {
            return texture && active.load(std::memory_order_acquire) &&
                passSnapshotsEnabled.load(std::memory_order_relaxed) &&
                finalFermiInput.load(std::memory_order_acquire) ==
                    reinterpret_cast<std::uintptr_t>(texture);
        }

        // Called only for sampled views of a draw whose color target is
        // the already discovered source of the final Fermi-to-VI blit.
        bool ObserveCompositionInput(const void *texture) {
            if (!texture || !active.load(std::memory_order_acquire) ||
                !passSnapshotsEnabled.load(std::memory_order_relaxed))
                return false;
            const auto id{reinterpret_cast<std::uintptr_t>(texture)};
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed)) return false;
            for (auto entry : compositionInputs)
                if (entry == id) return false;
            for (auto &entry : compositionInputs) {
                if (!entry) {
                    entry = id;
                    return true;
                }
            }
            return false;
        }

        bool IsCompositionInput(const void *texture) {
            if (!texture || !active.load(std::memory_order_acquire) ||
                !passSnapshotsEnabled.load(std::memory_order_relaxed))
                return false;
            const auto id{reinterpret_cast<std::uintptr_t>(texture)};
            std::scoped_lock lock(mutex);
            for (auto entry : compositionInputs)
                if (entry == id) return true;
            return false;
        }

        // Only called on sampled views of the pass which writes a texture
        // already proven to feed final HDR composition. No shader-hash or
        // guest-address hardcoding is involved.
        bool ObserveUpstreamInput(const void *texture, std::size_t surfaceBytes) {
            if (!texture || !active.load(std::memory_order_acquire) ||
                !passSnapshotsEnabled.load(std::memory_order_relaxed))
                return false;
            const auto id{reinterpret_cast<std::uintptr_t>(texture)};
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed)) return false;
            if (surfaceBytes > largestUpstreamBytes) {
                largestUpstreamBytes = surfaceBytes;
                primarySceneTexture.store(id, std::memory_order_release);
            }
            for (auto entry : upstreamInputs)
                if (entry == id) return false;
            for (auto &entry : upstreamInputs) {
                if (!entry) {
                    entry = id;
                    return true;
                }
            }
            return false;
        }

        bool IsPrimarySceneTexture(const void *texture) const {
            return texture && active.load(std::memory_order_acquire) &&
                sceneProducerOnly.load(std::memory_order_relaxed) &&
                primarySceneTexture.load(std::memory_order_acquire) ==
                    reinterpret_cast<std::uintptr_t>(texture);
        }

        bool IsUpstreamInput(const void *texture) {
            if (!texture || !active.load(std::memory_order_acquire) ||
                !passSnapshotsEnabled.load(std::memory_order_relaxed))
                return false;
            const auto id{reinterpret_cast<std::uintptr_t>(texture)};
            std::scoped_lock lock(mutex);
            for (auto entry : upstreamInputs)
                if (entry == id) return true;
            return false;
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
                // Discovery needs several VI intervals; scene-mode snapshots
                // start only after the identified scene target has been seen.
                (sceneProducerOnly.load(std::memory_order_relaxed) && interval < 4) ||
                interval >= FrameCount || bytes == 0 || bytes > MaxSingleImageBytes ||
                snapshotsByInterval[interval] >= 6 ||
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
