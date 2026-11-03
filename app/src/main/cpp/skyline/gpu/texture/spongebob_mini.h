// SPDX-License-Identifier: MPL-2.0
#pragma once

// Explicit, bounded Vulkan image snapshots for title-screen investigation.
// Does not acquire GPU locks, discard guest bytes, or change resource authority.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <ostream>
#include <utility>
#include <vector>

namespace skyline::gpu::texture::diagnostic {
    struct SpongeBobMiniBinding {
        uint32_t ticIndex{UINT32_MAX}; // Unknown if no matching TIC-cache entry.
        uint64_t iova{};
        uint32_t matchingEntries{};
    };

    struct SpongeBobMiniDrawInfo {
        uint64_t pipelineHash{}, channelSequence{};
        std::array<uint64_t, 6> shaderHashes{}; // Maxwell 3D pipeline stage order.
        uint32_t kind{}, topology{}, indexed{}, first{}, count{}, instances{};
        int32_t scissorX{}, scissorY{};
        uint32_t scissorWidth{}, scissorHeight{}, conditional{}, depthTest{};
        std::vector<SpongeBobMiniBinding> sampledBindings;
    };

    struct SpongeBobMiniInfo {
        uint64_t operation{};
        uint32_t role{}; // 1 target-before, 2 target-after, 3..6 sampled inputs
        uint64_t image{}, generation{}, writeSerial{};
        uint32_t hostFormat{}, guestFormat{};
        uint32_t width{}, height{}, x{}, y{}, regionWidth{}, regionHeight{};
        uint32_t mip{}, layer{}, blockWidth{}, blockHeight{}, bytesPerBlock{}, size{};
        uint64_t pipelineHash{}, channelSequence{};
        std::array<uint64_t, 6> shaderHashes{};
        uint32_t drawKind{}, topology{}, indexed{}, first{}, count{}, instances{};
        uint32_t sampledOrdinal{UINT32_MAX}, ticIndex{UINT32_MAX}, matchingTics{};
        uint64_t ticIova{};
        uint32_t captureScope{1}; // 1 = naturally ended render pass (SBMINI3)
    };

    // Candidate writers are recorded when a draw/clear is submitted to the
    // command executor. A candidate may be culled or write zero fragments.
    // Raw texture keys are process-local and are never dereferenced in history.
    struct SpongeBobWriter {
        uintptr_t textureKey{};
        uint64_t generation{};
        uintptr_t guestFirstBegin{};
        size_t guestFirstSize{};
        uint64_t pipeline{}, vertexShader{}, fragmentShader{}, computeShader{};
        uint64_t ordinal{}, channelSequence{};
        // 1=graphics, 2=storage-image subpass, 3=color clear,
        // 4=compute storage image, 5=Fermi2D blit, 6=unclassified
        // subpass, 7=Maxwell3D clear, 8=depth clear, 9=DMA copy,
        // 10=DMA fill. These are possible writers, not pixel proof.
        uint32_t renderPassIndex{}, kind{};
        uint32_t format{}, width{}, height{};
    };

    struct SpongeBobWriterMatch {
        SpongeBobWriter earliest{}, latest{}, firstIdentified{}, latestIdentified{};
        uint64_t matchingEvents{}, identifiedEvents{};
        bool exactTexture{}, truncated{};
        uint64_t resourcesEvicted{};
    };

    class SpongeBobMini {
        struct Entry {
            SpongeBobMiniInfo info;
            std::function<std::vector<uint8_t>()> read;
        };
        mutable std::mutex mutex;
        std::atomic<bool> armed{};
        std::atomic<bool> tracingNaturalPass{};
        // When armed, prefer a scene/HDR pass over a 1x1 white-quad overlay.
        // The filter has no effect on normal (disarmed) emulation.
        std::atomic<bool> preferScenePass{true};
        std::atomic<uint64_t> skippedOverlayPasses{};
        bool frozen{};
        uint64_t operations{};
        std::vector<Entry> entries;
        // Fixed-size PER-RESOURCE summaries: 1111+ writes to a few textures
        // must not evict the first/last writer of those textures. Diagnostic
        // activity starts only after the user arms it. No dynamic allocations
        // or GPU operations on the record path; only a bounded mutex section.
        struct WriterHistory {
            SpongeBobWriter earliest{}, latest{}, firstIdentified{}, latestIdentified{};
            uint64_t count{}, identifiedCount{};
        };
        static constexpr size_t MaxTrackedResources{512};
        std::array<WriterHistory, MaxTrackedResources> history{};
        uint64_t writerCount{}, resourcesEvicted{};
        size_t trackedResources{};
        static constexpr uint64_t MaxOperations{3};
        static constexpr size_t MaxEntries{MaxOperations * 6};

      public:
        bool Armed() const { return armed.load(std::memory_order_relaxed); }
        bool TracingNaturalPass() const { return tracingNaturalPass.load(std::memory_order_acquire); }
        bool NeedsDrawInfo() const { return Armed() || TracingNaturalPass(); }
        bool PreferScenePass() const { return preferScenePass.load(std::memory_order_acquire); }
        uint64_t SkippedOverlayPass() { return ++skippedOverlayPasses; }

        static bool HasShaderIdentity(const SpongeBobWriter &w) {
            return w.pipeline || w.vertexShader || w.fragmentShader || w.computeShader;
        }

        void NotePossibleWriter(SpongeBobWriter writer) {
            if (!Armed()) return;
            std::lock_guard lock{mutex};
            if (!armed.load(std::memory_order_relaxed) ||
                (!writer.textureKey && !writer.guestFirstBegin)) return;

            writer.ordinal = ++writerCount;
            WriterHistory *existing{}, *freeSlot{}, *oldest{};
            for (auto &slot : history) {
                if (!slot.count) {
                    if (!freeSlot) freeSlot = &slot;
                    continue;
                }
                const auto &key{slot.earliest};
                if (writer.textureKey
                    ? key.textureKey == writer.textureKey && key.generation == writer.generation
                    : !key.textureKey && key.guestFirstBegin == writer.guestFirstBegin) {
                    existing = &slot;
                    break;
                }
                if (!oldest || slot.latest.ordinal < oldest->latest.ordinal)
                    oldest = &slot;
            }
            if (!existing) {
                existing = freeSlot ? freeSlot : oldest;
                if (!existing) return;
                if (freeSlot) ++trackedResources;
                else ++resourcesEvicted;
                *existing = WriterHistory{};
                existing->earliest = writer;
            }
            existing->latest = writer;
            ++existing->count;
            if (HasShaderIdentity(writer)) {
                if (!existing->identifiedCount) existing->firstIdentified = writer;
                existing->latestIdentified = writer;
                ++existing->identifiedCount;
            }
        }

        SpongeBobWriterMatch FindPriorWriters(SpongeBobWriter resource) const {
            std::lock_guard lock{mutex};
            SpongeBobWriterMatch match{};
            match.resourcesEvicted = resourcesEvicted;
            match.truncated = resourcesEvicted != 0;
            const auto overlaps{[](const SpongeBobWriter &a, const SpongeBobWriter &b) {
                if (!a.guestFirstBegin || !b.guestFirstBegin ||
                    !a.guestFirstSize || !b.guestFirstSize) return false;
                if (a.guestFirstSize > std::numeric_limits<uintptr_t>::max() - a.guestFirstBegin ||
                    b.guestFirstSize > std::numeric_limits<uintptr_t>::max() - b.guestFirstBegin)
                    return false;
                return a.guestFirstBegin < b.guestFirstBegin + b.guestFirstSize &&
                       b.guestFirstBegin < a.guestFirstBegin + a.guestFirstSize;
            }};
            const auto merge{[&](const WriterHistory &slot) {
                if (!match.matchingEvents || slot.earliest.ordinal < match.earliest.ordinal)
                    match.earliest = slot.earliest;
                if (!match.matchingEvents || slot.latest.ordinal > match.latest.ordinal)
                    match.latest = slot.latest;
                if (slot.identifiedCount) {
                    if (!match.identifiedEvents ||
                        slot.firstIdentified.ordinal < match.firstIdentified.ordinal)
                        match.firstIdentified = slot.firstIdentified;
                    if (!match.identifiedEvents ||
                        slot.latestIdentified.ordinal > match.latestIdentified.ordinal)
                        match.latestIdentified = slot.latestIdentified;
                }
                match.matchingEvents += slot.count;
                match.identifiedEvents += slot.identifiedCount;
            }};
            if (resource.textureKey) {
                for (const auto &slot : history) {
                    if (slot.count && slot.earliest.textureKey == resource.textureKey &&
                        slot.earliest.generation == resource.generation) {
                        merge(slot);
                        match.exactTexture = true;
                        return match;
                    }
                }
            }
            // Only weaker alias hints when an exact texture/backing was not
            // observed. First guest mapping overlap is not complete alias
            // reconciliation and cannot establish actual pixel writes.
            for (const auto &slot : history)
                if (slot.count && overlaps(slot.earliest, resource))
                    merge(slot);
            return match;
        }

        uint64_t RecordedPossibleWriters() const {
            std::lock_guard lock{mutex};
            return writerCount;
        }

        size_t TrackedResources() const {
            std::lock_guard lock{mutex};
            return trackedResources;
        }

        uint64_t EvictedResources() const {
            std::lock_guard lock{mutex};
            return resourcesEvicted;
        }

        void EndNaturalPass() { tracingNaturalPass.store(false, std::memory_order_release); }

        bool Arm(bool preferScene = true) {
            std::lock_guard lock{mutex};
            if (frozen || armed || tracingNaturalPass || operations >= MaxOperations) return false;
            preferScenePass.store(preferScene, std::memory_order_release);
            skippedOverlayPasses.store(0, std::memory_order_release);
            writerCount = 0;
            resourcesEvicted = 0;
            trackedResources = 0;
            history = {};
            armed.store(true, std::memory_order_release);
            return true;
        }

        uint64_t Begin() {
            if (!Armed()) return 0;
            std::lock_guard lock{mutex};
            if (frozen || !armed || operations >= MaxOperations) return 0;
            armed.store(false, std::memory_order_release);
            tracingNaturalPass.store(true, std::memory_order_release);
            return ++operations;
        }

        bool Add(SpongeBobMiniInfo info, std::function<std::vector<uint8_t>()> read) {
            std::lock_guard lock{mutex};
            if (frozen || !info.operation || info.operation > operations ||
                info.role < 1 || info.role > 6 || !info.size || entries.size() >= MaxEntries)
                return false;
            for (const auto &entry : entries)
                if (entry.info.operation == info.operation && entry.info.role == info.role)
                    return false;
            entries.push_back({info, std::move(read)});
            return true;
        }

        bool Export(std::ostream &out) {
            std::vector<Entry> copy;
            {
                std::lock_guard lock{mutex};
                // Verify all record pairs before freezing. The Android export
                // action may be pressed while the last command is still queued.
                if (armed || tracingNaturalPass || entries.empty()) return false;
                for (uint64_t op{1}; op <= operations; ++op) {
                    const Entry *before{}, *after{};
                    unsigned inputs{};
                    for (const auto &entry : entries) {
                        if (entry.info.operation != op) continue;
                        if (entry.info.role == 1) before = &entry;
                        if (entry.info.role == 2) after = &entry;
                        if (entry.info.role >= 3) ++inputs;
                    }
                    if (!before || !after || !inputs ||
                        before->info.image != after->info.image ||
                        before->info.generation != after->info.generation ||
                        before->info.size != after->info.size)
                        return false;
                }
                frozen = true;
                copy = entries;
            }

            // No resource or controller lock is held across GPU fence waits.
            std::vector<std::vector<uint8_t>> payloads;
            payloads.reserve(copy.size());
            for (const auto &entry : copy) {
                auto bytes{entry.read()};
                if (bytes.size() != entry.info.size) return false;
                payloads.emplace_back(std::move(bytes));
            }
            out << "SBMINI3\n";
            for (size_t i{}; i < copy.size(); ++i) {
                const auto &m{copy[i].info};
                out << m.operation << ',' << m.role << ',' << m.image << ','
                    << m.generation << ',' << m.writeSerial << ',' << m.hostFormat << ','
                    << m.guestFormat << ',' << m.width << ',' << m.height << ','
                    << m.x << ',' << m.y << ',' << m.regionWidth << ',' << m.regionHeight
                    << ',' << m.mip << ',' << m.layer << ',' << m.blockWidth << ','
                    << m.blockHeight << ',' << m.bytesPerBlock << ',' << m.size
                    << ',' << m.pipelineHash << ',' << m.channelSequence;
                for (auto shaderHash : m.shaderHashes) out << ',' << shaderHash;
                out << ',' << m.drawKind << ',' << m.topology << ',' << m.indexed
                    << ',' << m.first << ',' << m.count << ',' << m.instances
                    << ',' << m.sampledOrdinal << ',' << m.ticIndex
                    << ',' << m.matchingTics << ',' << m.ticIova
                    << ',' << m.captureScope << '\n';
                out.write(reinterpret_cast<const char *>(payloads[i].data()), payloads[i].size());
                out.put('\n');
            }
            return out.good();
        }
    };
}
