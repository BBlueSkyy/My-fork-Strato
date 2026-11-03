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
        uint64_t pipeline{}, vertexShader{}, fragmentShader{};
        uint64_t ordinal{};
        uint32_t renderPassIndex{}, kind{}; // 1=color, 2=storage, 3=clear
        uint32_t format{}, width{}, height{};
    };

    struct SpongeBobWriterMatch {
        SpongeBobWriter earliest{}, latest{};
        uint32_t matchingEvents{};
        bool exactTexture{}, truncated{};
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
        // Fixed-size ring: writer tracing begins only after an explicit arm.
        // No GPU copies, allocations, or fence waits on the write path.
        static constexpr size_t MaxWriters{256};
        std::array<SpongeBobWriter, MaxWriters> writers{};
        uint64_t writerCount{};
        static constexpr uint64_t MaxOperations{3};
        static constexpr size_t MaxEntries{MaxOperations * 6};

      public:
        bool Armed() const { return armed.load(std::memory_order_relaxed); }
        bool TracingNaturalPass() const { return tracingNaturalPass.load(std::memory_order_acquire); }
        bool NeedsDrawInfo() const { return Armed() || TracingNaturalPass(); }
        bool PreferScenePass() const { return preferScenePass.load(std::memory_order_acquire); }
        uint64_t SkippedOverlayPass() { return ++skippedOverlayPasses; }

        void NotePossibleWriter(SpongeBobWriter writer) {
            if (!Armed()) return;
            std::lock_guard lock{mutex};
            if (!armed.load(std::memory_order_relaxed)) return;
            writer.ordinal = ++writerCount;
            writers[(writerCount - 1) % MaxWriters] = writer;
        }

        SpongeBobWriterMatch FindPriorWriters(SpongeBobWriter resource) const {
            SpongeBobWriterMatch match{};
            std::lock_guard lock{mutex};
            match.truncated = writerCount > MaxWriters;
            const auto available{std::min<uint64_t>(writerCount, MaxWriters)};
            const auto physicalOverlap{[](const SpongeBobWriter &a, const SpongeBobWriter &b) {
                if (!a.guestFirstBegin || !b.guestFirstBegin || !a.guestFirstSize || !b.guestFirstSize)
                    return false;
                if (a.guestFirstSize > std::numeric_limits<uintptr_t>::max() - a.guestFirstBegin ||
                    b.guestFirstSize > std::numeric_limits<uintptr_t>::max() - b.guestFirstBegin)
                    return false;
                return a.guestFirstBegin < b.guestFirstBegin + b.guestFirstSize &&
                       b.guestFirstBegin < a.guestFirstBegin + a.guestFirstSize;
            }};
            // Prefer exact texture+generation matches. A first guest mapping
            // overlap is weaker evidence of aliases, not guaranteed identity.
            for (int pass{}; pass != 2; ++pass) {
                match.matchingEvents = 0;
                for (uint64_t i{}; i < available; ++i) {
                    const auto &candidate{writers[(writerCount - available + i) % MaxWriters]};
                    const bool exact{candidate.textureKey == resource.textureKey &&
                                     candidate.generation == resource.generation};
                    if ((pass == 0 && !exact) ||
                        (pass == 1 && (exact || !physicalOverlap(candidate, resource))))
                        continue;
                    if (!match.matchingEvents) match.earliest = candidate;
                    match.latest = candidate;
                    ++match.matchingEvents;
                }
                if (match.matchingEvents) {
                    match.exactTexture = pass == 0;
                    break;
                }
            }
            return match;
        }

        uint64_t RecordedPossibleWriters() const {
            std::lock_guard lock{mutex};
            return writerCount;
        }

        void EndNaturalPass() { tracingNaturalPass.store(false, std::memory_order_release); }

        bool Arm(bool preferScene = true) {
            std::lock_guard lock{mutex};
            if (frozen || armed || tracingNaturalPass || operations >= MaxOperations) return false;
            preferScenePass.store(preferScene, std::memory_order_release);
            skippedOverlayPasses.store(0, std::memory_order_release);
            writerCount = 0;
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
