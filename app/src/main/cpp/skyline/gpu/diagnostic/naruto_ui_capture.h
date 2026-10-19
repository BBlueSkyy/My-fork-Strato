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
    // Shape-only portion of the opt-in RGBA8 text-strip filter. Unlike the
    // original 1024..1280 width limit, this also handles larger internal
    // framebuffer resolutions (e.g. 1664x64/96 from a 1600x900 output).
    // This is NOT a production texture classification or a game-specific fix.
    constexpr bool IsWideTextStripGeometry(std::uint32_t width, std::uint32_t height) {
        return width >= 512 && width <= 2048 &&
               height >= 16 && height <= 192 &&
               width >= 6 * height;
    }

    class UiFrameCapture {
      public:
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> presentIndex{0};
        std::atomic<std::uint32_t> eventCount{0};
        std::atomic<std::uint32_t> sampledDrawEvents{0};
        std::atomic<std::uint64_t> nextFocusedDrawId{1};
        static constexpr std::uint32_t MaxEvents{6000};
        static constexpr std::uint32_t FrameCount{4}; // Full draw-pair probes only.
        // Continue source frames for roughly one second, because sporadic UI
        // flickers can be missed by a ~120 ms four-frame presentation window.
        // Outside FrameCount the detailed per-draw quota remains inactive.
        static constexpr std::uint32_t PresentFrameCount{24};
        // At most one full-sized input to the first full-screen inverted blit
        // per presentation-sized interval. This is distinct from draw probes.
        static constexpr std::uint32_t MaxFermiSourceSnapshots{24};
        static constexpr std::size_t MaxFermiSourceBytes{72ULL * 1024 * 1024};
        // One diagnostic intermediate RT immediately before the first
        // full-resolution text consumer per completed composition cycle.
        static constexpr std::uint32_t MaxUiMidpointSnapshots{24};
        static constexpr std::size_t MaxUiMidpointBytes{72ULL * 1024 * 1024};
        // Retain the existing budget for a post-fourth-text result rather
        // than the already-understood pre-first-text image. Both snapshots
        // would double diagnostic cost without improving localization.
        static constexpr std::uint32_t MaxPostFourthTextSnapshots{MaxUiMidpointSnapshots};
        // 24 x 1600x900 RGBA8 = 138,240,000 bytes. Previous 72 MiB
        // stopped after 13 frames and missed faults occurring later in the
        // 24-frame presentation window. This larger quota is only for the
        // manually armed post-text probe, not production rendering.
        static constexpr std::size_t MaxPostFourthTextBytes{144ULL * 1024 * 1024};
        // Reserve budget for late UI composition and each of the four intervals.
        // Phase 6 only needs the first UI draw before/after images. Keep one
        // atlas per interval; the shader/CB/TSC evidence is tiny by comparison.
        static constexpr std::uint32_t PassSnapshotsPerInterval{6};
        static constexpr std::uint32_t MaxPassSnapshots{24};
        static constexpr std::uint32_t AtlasSnapshotsPerInterval{2};
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
        // Independent quotas for glyph-RT writer, strip compositor, and full-size consumer.
        std::array<std::array<std::uint32_t, 4>, FrameCount> textFocusCounts{};
        std::array<bool, FrameCount> preUiDrawClaimed{};
        std::array<bool, FrameCount> preSpriteDrawClaimed{};
        std::optional<std::pair<std::uintptr_t, std::uintptr_t>> fermiSourceDirection;
        // Live backing identities actually written by a qualifying
        // full-resolution UI consumer draw. The first Fermi blit receives
        // background only; its reverse direction reads this composed target.
        std::vector<std::uintptr_t> uiCompositorTargets;
        // Kept behind the diagnostic mutex. Zero=eligible for pre-text probe,
        // one=already claimed for this composition cycle.
        std::vector<std::pair<std::uintptr_t, std::uint8_t>> uiTextConsumersByTarget;
        std::uint32_t uiMidpointSnapshotCount{};
        std::size_t uiMidpointBytes{};
        // Reset when Fermi2D consumes the completed full-resolution UI.
        // Maxwell3D may split one composition cycle across submissions.
        struct TextDrawCycle {
            std::uintptr_t target{};
            std::uint8_t draws{};
        };
        std::vector<TextDrawCycle> textDrawCycles;
        std::uint32_t postFourthTextCount{};
        std::size_t postFourthTextBytes{};
        std::uint32_t fermiSourceSnapshotCount{};
        std::size_t fermiSourceBytes{};
        // One full-surface vertically flipped blit destination per approximate
        // presentation interval. Discover at runtime; never key on a game ID.
        std::array<std::pair<std::uintptr_t, std::size_t>, FrameCount> flippedDestinations{};
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
            textFocusCounts.fill({});
            preUiDrawClaimed.fill(false);
            preSpriteDrawClaimed.fill(false);
            fermiSourceDirection.reset();
            uiCompositorTargets.clear();
            uiTextConsumersByTarget.clear();
            uiMidpointSnapshotCount = 0;
            uiMidpointBytes = 0;
            textDrawCycles.clear();
            postFourthTextCount = 0;
            postFourthTextBytes = 0;
            fermiSourceSnapshotCount = 0;
            fermiSourceBytes = 0;
            flippedDestinations.fill({});
            capturedAtlasIdentities.clear();
            nextPassIndex = 0;
            passBytes = 0;
            watchedMappings.clear();
            eventCount.store(0, std::memory_order_relaxed);
            sampledDrawEvents.store(0, std::memory_order_relaxed);
            nextFocusedDrawId.store(1, std::memory_order_relaxed);
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

        void MarkFlippedDestination(std::uint32_t interval, std::uintptr_t guestAddress, std::size_t bytes) {
            if (!guestAddress || !bytes || interval >= FrameCount ||
                !active.load(std::memory_order_acquire))
                return;
            std::scoped_lock lock(mutex);
            if (active.load(std::memory_order_relaxed))
                flippedDestinations[interval] = {guestAddress, bytes};
        }

        bool IsFlippedDestination(std::uint32_t interval, std::uintptr_t guestAddress, std::size_t bytes) {
            if (!guestAddress || !bytes || interval >= FrameCount ||
                !active.load(std::memory_order_acquire))
                return false;
            std::scoped_lock lock(mutex);
            const auto [start, length]{flippedDestinations[interval]};
            if (!start || !length)
                return false;
            return guestAddress >= start ? guestAddress - start < length
                                         : start - guestAddress < bytes;
        }

        // Reused diagnostic quotas: FullConsumer=blended scene draw, FullSprite=opaque scene draw.\n        enum class TextFocus : std::uint8_t { GlyphOutput, StripComposite, FullConsumer, FullSprite };

        // These scopes are selected only after a render target/source has been
        // identified by its format/geometry while capture is armed. They are not
        // based on the game ID or specific shader hash. This reserves the budget
        // for actual text operations instead of unrelated 1120x630 effects.
        std::optional<std::uint32_t> ClaimTextFocus(std::uint32_t interval, TextFocus kind) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || interval >= FrameCount)
                return std::nullopt;
            // Spend readbacks on the actual full-resolution consumers, not upstream glyphs
            // already validated in the preceding capture.
            constexpr std::array<std::uint32_t, 4> limits{0, 0, 1, 1};
            const auto stage{static_cast<std::size_t>(kind)};
            auto &number{textFocusCounts[interval][stage]};
            if (number >= limits[stage])
                return std::nullopt;
            return ++number;
        }

        std::uint64_t NextFocusedDrawId() {
            return nextFocusedDrawId.fetch_add(1, std::memory_order_relaxed);
        }

        // The host texture identity is observed as an *actual render target*
        // from Maxwell3D; no game ID, shader hash, magic guest IOVA, screen ROI
        // or driver name participates in selecting the Fermi source.
        void ObserveUiCompositorTarget(std::uintptr_t identity) {
            if (!identity || !active.load(std::memory_order_acquire))
                return;
            std::scoped_lock lock(mutex);
            if (active.load(std::memory_order_relaxed) &&
                uiCompositorTargets.size() < 16 &&
                std::find(uiCompositorTargets.begin(), uiCompositorTargets.end(), identity) ==
                    uiCompositorTargets.end())
                uiCompositorTargets.push_back(identity);
        }

        // Observed trace: exactly ONE qualifying full-sized text-strip
        // consumer per compositor cycle. Capture the target BEFORE that draw,
        // after the preceding sprite draws. Do not require a second consumer:
        // that previously yielded zero snapshots.
        std::optional<std::uint32_t> ClaimUiBeforeText(
            std::uintptr_t targetIdentity, std::size_t bytes) {
            if (!targetIdentity || !active.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed))
                return std::nullopt;
            auto it{std::find_if(uiTextConsumersByTarget.begin(), uiTextConsumersByTarget.end(),
                [targetIdentity](const auto &entry) { return entry.first == targetIdentity; })};
            if (it == uiTextConsumersByTarget.end()) {
                if (uiTextConsumersByTarget.size() >= 16)
                    return std::nullopt;
                it = uiTextConsumersByTarget.emplace(
                    uiTextConsumersByTarget.end(), targetIdentity, 0);
            }
            if (it->second != 0)
                return std::nullopt;
            it->second = 1; // Mark claimed for this cycle, even if out of quota.
            if (!bytes || uiMidpointSnapshotCount >= MaxUiMidpointSnapshots ||
                uiMidpointBytes > MaxUiMidpointBytes ||
                bytes > MaxUiMidpointBytes - uiMidpointBytes)
                return std::nullopt;
            uiMidpointBytes += bytes;
            return uiMidpointSnapshotCount++;
        }

        // Count actual resolved text-strip draws to a *single* compositor
        // target between successive full-screen reverse Fermi2D copies.
        // Trace shows six or seven consumers per full composition cycle.
        // The fourth draw follows the first large sprite block and helps
        // isolate where the later P1 list is composed. Never group these
        // by approximate VI frame or Maxwell submission.
        std::optional<std::uint32_t> ClaimPostFourthText(
            std::uintptr_t target, std::size_t bytes) {
            if (!target || !active.load(std::memory_order_acquire))
                return std::nullopt;
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_relaxed))
                return std::nullopt;
            auto it{std::find_if(textDrawCycles.begin(), textDrawCycles.end(),
                [target](const TextDrawCycle &entry) { return entry.target == target; })};
            if (it == textDrawCycles.end()) {
                if (textDrawCycles.size() < 16)
                    textDrawCycles.push_back({target, 1});
                return std::nullopt;
            }
            // 0 (new cycle) -> 1 -> 2 -> 3 -> 4 (checkpoint);
            // ignore subsequent text draws until reverse Fermi.
            if (it->draws < 3) {
                ++it->draws;
                return std::nullopt;
            }
            if (it->draws != 3)
                return std::nullopt;
            it->draws = 4; // Exactly one probe per cycle.
            if (!bytes || postFourthTextCount >= MaxPostFourthTextSnapshots ||
                postFourthTextBytes > MaxPostFourthTextBytes ||
                bytes > MaxPostFourthTextBytes - postFourthTextBytes)
                return std::nullopt;
            postFourthTextBytes += bytes;
            return postFourthTextCount++;
        }

        // A pre-reverse Fermi probe ends this compositor cycle. Re-arm the
        // first pre-text capture of the next cycle.
        void CompleteUiCompositorCycle(std::uintptr_t targetIdentity) {
            if (!active.load(std::memory_order_acquire))
                return;
            std::scoped_lock lock(mutex);
            for (auto &entry : uiTextConsumersByTarget)
                if (entry.first == targetIdentity)
                    entry.second = 0;
            for (auto &entry : textDrawCycles)
                if (entry.target == targetIdentity)
                    entry.draws = 0;
        }

        // Only snapshot an image we saw receiving actual UI compositor draws,
        // never the first background-only Fermi input. Lock the chosen reverse
        // direction after its first match so subsequent observations agree.
        std::optional<std::uint32_t> ReserveFermiSource(
            std::uintptr_t sourceIdentity, std::uintptr_t destinationIdentity, std::size_t bytes) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || !sourceIdentity || !destinationIdentity ||
                sourceIdentity == destinationIdentity || !bytes ||
                fermiSourceSnapshotCount >= MaxFermiSourceSnapshots ||
                std::find(uiCompositorTargets.begin(), uiCompositorTargets.end(), sourceIdentity) ==
                    uiCompositorTargets.end() ||
                fermiSourceBytes > MaxFermiSourceBytes ||
                bytes > MaxFermiSourceBytes - fermiSourceBytes)
                return std::nullopt;
            const auto direction{std::make_pair(sourceIdentity, destinationIdentity)};
            if (fermiSourceDirection && *fermiSourceDirection != direction)
                return std::nullopt;
            if (!fermiSourceDirection)
                fermiSourceDirection = direction;
            fermiSourceBytes += bytes;
            return fermiSourceSnapshotCount++;
        }

        // Independent pre-draw quotas for the first text and compressed-sprite consumers.
        bool ClaimPreSpriteDraw(std::uint32_t interval) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || interval >= FrameCount ||
                preSpriteDrawClaimed[interval])
                return false;
            preSpriteDrawClaimed[interval] = true;
            return true;
        }

        // Pre-draw readback on the first matching glyph writer, not a fixed
        // ordinal from an unrelated compositor. At most once per interval.
        bool ClaimPreUiDraw(std::uint32_t interval) {
            std::scoped_lock lock(mutex);
            if (!active.load(std::memory_order_acquire) || interval >= FrameCount ||
                preUiDrawClaimed[interval])
                return false;
            preUiDrawClaimed[interval] = true;
            return true;
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
            // Routine draws generated most of the 2400 earlier messages. Sample
            // them 1/16; never sample shader, sampler, Fermi2D or frame events.
            if (event.rfind("DRAW ", 0) == 0 &&
                (sampledDrawEvents.fetch_add(1, std::memory_order_relaxed) % 16) != 0)
                return;
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
                out << "GPU events collected while 24 frames were being presented.\n"
                    << "present_index_approx is NOT exact GPU/present synchronization.\n"
                    << "Events are capped at 6000; routine draws sampled 1/16.\n\n";
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
