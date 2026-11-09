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
        static constexpr std::uint32_t MaxSceneStateRecords{96};
        static constexpr std::uint32_t MaxOcclusionQueriesPerClass{12};
        static constexpr std::uint32_t MaxPassSnapshots{12};
        static constexpr std::size_t MaxPassCaptureBytes{64ULL * 1024 * 1024};
        static constexpr std::size_t MaxSingleImageBytes{12ULL * 1024 * 1024};

        std::atomic<bool> active{false};
        std::atomic<bool> passSnapshotsEnabled{false};
        // Separate optional stage: reserve the snapshot budget for the
        // largest live input to postprocessing, on subsequent world passes.
        std::atomic<bool> sceneProducerOnly{false};
        std::atomic<std::uintptr_t> primarySceneTexture{0};
        std::atomic<std::uint32_t> sceneDrawStateCandidates{0};
        // Separate, opt-in depth-plane snapshots. Existing color capture
        // behavior remains unchanged when the marker is not present.
        std::atomic<bool> sceneDepthReadbacksEnabled{false};
        std::atomic<bool> sceneOcclusionEnabled{false};
        // Persistent only while an explicit experiment marker exists.
        // This is separate from the six-frame capture lifetime so the
        // user can see the visual A/B result in normal gameplay.
        std::atomic<bool> depthEqualAlwaysExperiment{false};
        // Independent, reversible diagnostic. The cull-none experiment
        // takes precedence over depth ALWAYS if both markers are present.
        std::atomic<bool> sceneCullNoneExperiment{false};
        // Explicit third experiment to isolate the interaction of depth
        // comparison and face culling; inactive by default.
        std::atomic<bool> sceneDepthCullBypassExperiment{false};
        std::atomic<std::uint32_t> combinedPipelineUpdates{0};
        std::atomic<std::uint32_t> combinedDraws{0};
        std::atomic<std::uint32_t> depthAlwaysPipelineUpdates{0};
        std::atomic<std::uint32_t> depthAlwaysDraws{0};
        std::atomic<std::uint32_t> cullNonePipelineUpdates{0};
        std::atomic<std::uint32_t> cullNoneDraws{0};
        // Diagnostic-only counters explaining why a query was not selected.
        std::atomic<std::uint32_t> occlusionFilterLogged{0};
        std::atomic<std::uint32_t> occlusionSceneDraws{0};
        std::atomic<std::uint32_t> occlusionMatchedDepth{0};
        std::atomic<std::uint32_t> occlusionMatchedColorEqual{0};
        std::atomic<std::uint32_t> occlusionMatchedPrepass{0};
        std::atomic<std::uint32_t> occlusionGuestCounterRequested{0};
        std::atomic<std::uint32_t> occlusionGuestQueryActive{0};
        std::atomic<std::uintptr_t> sceneDepthTexture{0};
        // The EOW scene ping-pongs between two depth attachments.
        // Remember the previous one for accurate observation tagging.
        std::atomic<std::uintptr_t> previousSceneDepthTexture{0};
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
        std::uint32_t occlusionRequested{};
        std::uint32_t occlusionCompleted{};
        std::uint32_t occlusionFailed{};
        std::uint32_t occlusionUnavailable{};
        std::uint32_t occlusionPending{};
        std::array<std::uint32_t, 2> occlusionCandidates{};
        std::array<std::uint32_t, 2> occlusionSelected{};
        std::uint32_t depthSnapshotCount{};
        std::uint32_t colorSnapshotCount{};
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

        void MaybeWriteOcclusionCompletionLocked() {
            if (!finishedPresentCapture || occlusionPending) return;
            std::ofstream out(std::filesystem::path(directory) /
                              "occlusion_readbacks_done.txt");
            if (out)
                out << "requested=" << occlusionRequested << "\n"
                    << "written=" << occlusionCompleted << "\n"
                    << "failed=" << occlusionFailed << "\n"
                    << "unavailable=" << occlusionUnavailable << "\n"
                    << "prepass_selected=" << occlusionSelected[0] << "\n"
                    << "color_selected=" << occlusionSelected[1] << "\n"
                    << "scene_indexed_draws=" << occlusionSceneDraws.load() << "\n"
                    << "scene_with_tracked_depth=" << occlusionMatchedDepth.load() << "\n"
                    << "color_equal_candidates=" << occlusionMatchedColorEqual.load() << "\n"
                    << "depth_prepass_candidates=" << occlusionMatchedPrepass.load() << "\n"
                    << "candidates_guest_counter_enabled=" << occlusionGuestCounterRequested.load() << "\n"
                    << "candidates_guest_query_active=" << occlusionGuestQueryActive.load() << "\n";
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
                   bool sceneProducerCapture = false,
                   bool depthReadbacks = false, bool occlusionProbes = false) {
            std::scoped_lock lock(mutex);
            active.store(false, std::memory_order_release);
            DisarmSceneGate();
            currentPresentSource.store(0, std::memory_order_relaxed);
            finalFermiInput.store(0, std::memory_order_relaxed);
            primarySceneTexture.store(0, std::memory_order_relaxed);
            sceneDrawStateCandidates.store(0, std::memory_order_relaxed);
            sceneDepthTexture.store(0, std::memory_order_relaxed);
            previousSceneDepthTexture.store(0, std::memory_order_relaxed);
            sceneDepthReadbacksEnabled.store(
                sceneProducerCapture && capturePassSnapshots && depthReadbacks,
                std::memory_order_relaxed);
            sceneOcclusionEnabled.store(
                sceneProducerCapture && capturePassSnapshots && occlusionProbes,
                std::memory_order_relaxed);
            depthAlwaysPipelineUpdates.store(0, std::memory_order_relaxed);
            depthAlwaysDraws.store(0, std::memory_order_relaxed);
            cullNonePipelineUpdates.store(0, std::memory_order_relaxed);
            cullNoneDraws.store(0, std::memory_order_relaxed);
            combinedPipelineUpdates.store(0, std::memory_order_relaxed);
            combinedDraws.store(0, std::memory_order_relaxed);
            occlusionFilterLogged.store(0, std::memory_order_relaxed);
            occlusionSceneDraws.store(0, std::memory_order_relaxed);
            occlusionMatchedDepth.store(0, std::memory_order_relaxed);
            occlusionMatchedColorEqual.store(0, std::memory_order_relaxed);
            occlusionMatchedPrepass.store(0, std::memory_order_relaxed);
            occlusionGuestCounterRequested.store(0, std::memory_order_relaxed);
            occlusionGuestQueryActive.store(0, std::memory_order_relaxed);
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
            occlusionRequested = occlusionCompleted = occlusionFailed =
                occlusionUnavailable = occlusionPending = 0;
            occlusionCandidates.fill(0);
            occlusionSelected.fill(0);
            depthSnapshotCount = colorSnapshotCount = 0;
            sourceRequested = sourceWritten = sourceFailed = 0;
            finishedPresentCapture = false;
            presentIndex.store(0, std::memory_order_relaxed);
            active.store(true, std::memory_order_release);
        }

        bool CanStartCapture() {
            std::scoped_lock lock(mutex);
            return !active.load(std::memory_order_acquire) &&
                   pendingSnapshots == 0 && pendingSources.empty() &&
                   occlusionPending == 0;
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

        // At most 96 passive state records, sampled evenly across the first
        // 288 indexed scene draws instead of exhausting the event budget on
        // one subpass. No Vk commands/queries are issued here.
        bool ShouldRecordSceneDrawState(const void *target, bool indexed,
                                        std::uint32_t count) {
            if (!indexed || count < 96 || !IsPrimarySceneTexture(target))
                return false;
            const auto n{sceneDrawStateCandidates.fetch_add(1, std::memory_order_relaxed)};
            return n < MaxSceneStateRecords * 3 && n % 3 == 0;
        }

        bool ObserveSceneDepthTexture(const void *texture) {
            if (!texture || !active.load(std::memory_order_acquire) ||
                !(sceneDepthReadbacksEnabled.load(std::memory_order_relaxed) ||
                  sceneOcclusionEnabled.load(std::memory_order_relaxed)))
                return false;
            const auto ptr{reinterpret_cast<std::uintptr_t>(texture)};
            const auto last{sceneDepthTexture.exchange(ptr, std::memory_order_acq_rel)};
            if (last == ptr)
                return false;
            previousSceneDepthTexture.store(last, std::memory_order_release);
            return true;
        }

        bool IsSceneDepthTexture(const void *texture) const {
            if (!texture || !active.load(std::memory_order_acquire) ||
                !(sceneDepthReadbacksEnabled.load(std::memory_order_relaxed) ||
                  sceneOcclusionEnabled.load(std::memory_order_relaxed)))
                return false;
            const auto ptr{reinterpret_cast<std::uintptr_t>(texture)};
            return sceneDepthTexture.load(std::memory_order_acquire) == ptr ||
                   previousSceneDepthTexture.load(std::memory_order_acquire) == ptr;
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

        // Record why a sampled indexed draw could not receive a Vulkan
        // occlusion query. This is metadata only and does not relax the
        // guest query nesting protection. The summary survives Finish().
        void NoteOcclusionEligibility(bool sceneDraw, bool depthTracked,
                                      bool isPrepass, bool isColorEqual,
                                      bool guestCounterEnabled, bool guestQueryOpen,
                                      std::string detail) {
            if (!sceneOcclusionEnabled.load(std::memory_order_relaxed) ||
                !active.load(std::memory_order_acquire))
                return;
            if (sceneDraw) ++occlusionSceneDraws;
            if (depthTracked) ++occlusionMatchedDepth;
            if (isPrepass) ++occlusionMatchedPrepass;
            if (isColorEqual) ++occlusionMatchedColorEqual;
            if ((isPrepass || isColorEqual) && guestCounterEnabled)
                ++occlusionGuestCounterRequested;
            if ((isPrepass || isColorEqual) && guestQueryOpen)
                ++occlusionGuestQueryActive;
            if ((sceneDraw || depthTracked) &&
                occlusionFilterLogged.fetch_add(1, std::memory_order_relaxed) < 32)
                Record(std::move(detail));
        }

        // A few independent occlusion queries for matched scene passes.
        // No guest query pool, guest report buffer or GPU state is reused.
        // IDs and completion counts are kept outside the bounded event log
        // because callbacks run after the capture window closes.
        std::optional<std::uint32_t> ReserveOcclusionQuery(bool depthPrepass) {
            if (!active.load(std::memory_order_acquire) ||
                !sceneOcclusionEnabled.load(std::memory_order_relaxed))
                return std::nullopt;
            const auto interval{presentIndex.load(std::memory_order_acquire)};
            if (interval < 4 || interval >= FrameCount) return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed)) return std::nullopt;
            const std::size_t cls{depthPrepass ? 0U : 1U};
            const auto ordinal{occlusionCandidates[cls]++};
            // Capture a representative spread of the existing draws.
            if (ordinal % 4 || occlusionSelected[cls] >= MaxOcclusionQueriesPerClass)
                return std::nullopt;
            ++occlusionSelected[cls];
            ++occlusionPending;
            return occlusionRequested++;
        }

        void OcclusionQueryFinished(bool success, bool available) {
            std::scoped_lock lock(mutex);
            if (!occlusionPending) return;
            --occlusionPending;
            if (success) ++occlusionCompleted;
            else ++occlusionFailed;
            if (success && !available) ++occlusionUnavailable;
            MaybeWriteOcclusionCompletionLocked();
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
                                                      std::size_t bytes, bool depthPlane = false) {
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
            if (sceneDepthReadbacksEnabled.load(std::memory_order_relaxed)) {
                // Six passes max, split evenly across color and depth;
                // the existing global 12-snapshot/64-MiB caps still apply.
                if (depthPlane ? depthSnapshotCount >= 3 : colorSnapshotCount >= 3)
                    return std::nullopt;
                if (depthPlane) ++depthSnapshotCount;
                else ++colorSnapshotCount;
            }
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
            // Selection counts are read while the capture is still active;
            // the experiment itself continues until the marker is removed.
            std::ofstream experimentOut(
                std::filesystem::path(directory) / "depth_always_experiment.txt",
                std::ios::trunc);
            if (experimentOut)
                experimentOut << "enabled_during_finish="
                              << depthEqualAlwaysExperiment.load(std::memory_order_relaxed)
                              << "\nmatched_pipeline_updates="
                              << depthAlwaysPipelineUpdates.load(std::memory_order_relaxed)
                              << "\nmatched_scene_draws="
                              << depthAlwaysDraws.load(std::memory_order_relaxed)
                              << "\nnote=diagnostic_only_original_pipeline_preserved\n";
            std::ofstream cullExperimentOut(
                std::filesystem::path(directory) / "scene_cull_none_experiment.txt",
                std::ios::trunc);
            if (cullExperimentOut)
                cullExperimentOut << "enabled_during_finish="
                                  << sceneCullNoneExperiment.load(std::memory_order_relaxed)
                                  << "\nmatched_pipeline_updates="
                                  << cullNonePipelineUpdates.load(std::memory_order_relaxed)
                                  << "\nmatched_scene_draws="
                                  << cullNoneDraws.load(std::memory_order_relaxed)
                                  << "\nnote=diagnostic_only_original_cull_pipeline_preserved\n";
            std::ofstream combinedOut(
                std::filesystem::path(directory) / "scene_depth_cull_bypass_experiment.txt",
                std::ios::trunc);
            if (combinedOut)
                combinedOut << "enabled_during_finish="
                            << sceneDepthCullBypassExperiment.load(std::memory_order_relaxed)
                            << "\nmatched_pipeline_updates="
                            << combinedPipelineUpdates.load(std::memory_order_relaxed)
                            << "\nmatched_scene_draws="
                            << combinedDraws.load(std::memory_order_relaxed)
                            << "\nnote=diagnostic_only_distinct_pipeline_variant\n";
            finishedPresentCapture = true;
            MaybeWriteCompletionLocked();
            MaybeWriteSourceCompletionLocked();
            MaybeWriteOcclusionCompletionLocked();
        }
    };

    inline EowCaptureState &EowCapture() {
        static EowCaptureState session;
        return session;
    }
}
