// SPDX-License-Identifier: MPL-2.0
#pragma once

// Temporary, bounded TIC diagnostics. No GPU-visible state or caches are changed.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <mutex>
#include <optional>
#include <type_traits>

namespace skyline::gpu::interconnect::tic_probe {
    struct ShaderContext {
        const char *kind{"runtime"};
        std::uint32_t stage{};
        std::uint64_t hash{};
    };

    inline thread_local ShaderContext currentShader{};

    // The shader compiler describes the requested type; descriptor binding
    // later reads the actual bindless handle from a guest constant buffer.
    // Keep only a thread-local breadcrumb for TIC index zero. It lets a
    // reserved sampled TIC distinguish an unbound CBUF synthesized as zero
    // by Strato from an actual zero read from the guest's bound CBUF.
    struct BindlessSource {
        std::uint64_t channelSequence{};
        std::uint64_t primaryOffset{};
        std::uint32_t raw{};
        std::uint32_t primaryValue{};
        std::uint32_t secondaryValue{};
        std::uint32_t primaryIndex{};
        std::uint32_t arrayIndex{};
        bool valid{};
        bool primaryBound{};
        bool secondaryUsed{};
        bool secondaryBound{};
    };
    inline thread_local BindlessSource lastBindless{};

    inline void RememberZeroBindless(std::uint64_t sequence, std::uint32_t raw,
                                    std::uint32_t primaryIndex, std::uint64_t primaryOffset,
                                    std::uint32_t arrayIndex, bool primaryBound,
                                    std::uint32_t primaryValue, bool secondaryUsed,
                                    bool secondaryBound, std::uint32_t secondaryValue) {
        if ((raw & 0xFFFFFu) != 0) {
            lastBindless.valid = false;
            return;
        }
        lastBindless = {sequence, primaryOffset, raw, primaryValue, secondaryValue,
                        primaryIndex, arrayIndex, true, primaryBound, secondaryUsed,
                        secondaryBound};
    }

    struct ScopedShaderContext {
        ShaderContext previous;
        ScopedShaderContext(const char *kind, std::uint32_t stage, std::uint64_t hash)
            : previous{currentShader} {
            currentShader = {kind, stage, hash};
        }
        ~ScopedShaderContext() {
            currentShader = previous;
        }
        ScopedShaderContext(const ScopedShaderContext &) = delete;
        ScopedShaderContext &operator=(const ScopedShaderContext &) = delete;
    };

    template<typename T>
    std::array<std::uint32_t, 8> SnapshotWords(const T &descriptor) {
        static_assert(sizeof(T) == 32 && std::is_trivially_copyable_v<T>);
        std::array<std::uint32_t, 8> words{};
        // Copy the 32 descriptor bytes already read by the existing TIC path.
        // Never dereference the guest image IOVA to obtain the descriptor.
        std::memcpy(words.data(), &descriptor, sizeof(descriptor));
        return words;
    }

    struct ReservedSlot {
        std::uint64_t poolBase{};
        std::uint32_t index{};
        bool active{};
    };

    inline thread_local std::array<ReservedSlot, 32> reservedSlots{};
    inline thread_local std::uint32_t nextReservedSlot{};
    inline thread_local bool anyReservedSlots{};

    inline void RememberReserved(std::uint64_t poolBase, std::uint32_t index) {
        for (auto &entry : reservedSlots)
            if (entry.active && entry.poolBase == poolBase && entry.index == index)
                return;
        reservedSlots[nextReservedSlot++ % reservedSlots.size()] = {poolBase, index, true};
        anyReservedSlots = true;
    }

    inline bool ConsumeRecovered(std::uint64_t poolBase, std::uint32_t index) {
        if (!anyReservedSlots)
            return false;
        for (auto &entry : reservedSlots) {
            if (entry.active && entry.poolBase == poolBase && entry.index == index) {
                entry.active = false;
                return true;
            }
        }
        return false;
    }

    // Deduplicate equivalent events on each thread and cap log volume globally.
    // The cap applies only to the rare invalid-TIC path, never to normal draws.
    // A single lightweight observation target: TIC index 0 of the most
    // recently refreshed pool. This is the slot found in the Kirby capture,
    // but the watched IOVA comes from the current guest registers.
    // It is not a memory watchpoint and does not track CPU/NCE writes.
    inline std::atomic<std::uint64_t> watchedFirstTicIova{0};

    inline void WatchFirstTic(std::uint64_t iova) {
        watchedFirstTicIova.store(iova, std::memory_order_relaxed);
    }

    inline bool Overlaps(std::uint64_t start, std::uint64_t size,
                         std::uint64_t target, std::uint64_t targetSize = 32) {
        if (!size || !targetSize)
            return false;
        // Subtraction comparisons cannot overflow if near the 40-bit VA limit.
        return start <= target ? target - start < size : start - target < targetSize;
    }

    inline bool OverlapsWatchedFirstTic(std::uint64_t start, std::uint64_t size) {
        const auto ticIova{watchedFirstTicIova.load(std::memory_order_relaxed)};
        return ticIova && Overlaps(start, size, ticIova);
    }

    // Diagnostic-only association between the GPU IOVA range installed by
    // nvhost-as-gpu and the exact host backing passed into GMMU::Map.
    // This is a bounded, process-local history, not a memory trap. A matching
    // host pointer is REQUIRED to avoid attributing a replaced mapping to a
    // stale NvMap record.
    enum class MappingSource : std::uint32_t {
        Unknown = 0,
        Fixed = 1,
        Dynamic = 2,
        SubregionRemap = 3,
        SparseRemap = 4
    };
    struct MappingEvidence {
        const void *addressSpace{};
        std::uint64_t gpuBase{};
        std::uint64_t bytes{};
        std::uint64_t nvMapBackingBase{};
        std::uintptr_t hostBase{};
        std::uint64_t generation{};
        std::uint32_t nvMapId{};
        MappingSource source{MappingSource::Unknown};
        bool active{};
    };
    inline constexpr std::size_t MappingEvidenceLimit{4096};
    inline std::mutex mappingEvidenceMutex;
    inline std::array<MappingEvidence, MappingEvidenceLimit> mappingEvidence{};
    inline std::uint64_t nextMappingGeneration{};

    inline void RememberMapping(const void *addressSpace, std::uint64_t gpuBase,
                                std::uint64_t bytes, std::uintptr_t hostBase,
                                std::uint64_t nvMapBackingBase, std::uint32_t nvMapId,
                                MappingSource source) {
        if (!addressSpace || !bytes || !hostBase)
            return;
        std::scoped_lock lock{mappingEvidenceMutex};
        const auto generation{++nextMappingGeneration};
        mappingEvidence[(generation - 1) % MappingEvidenceLimit] = {
            addressSpace, gpuBase, bytes, nvMapBackingBase, hostBase,
            generation, nvMapId, source, true
        };
    }

    inline void ForgetMappingRange(const void *addressSpace, std::uint64_t gpuBase,
                                   std::uint64_t bytes) {
        if (!addressSpace || !bytes)
            return;
        std::scoped_lock lock{mappingEvidenceMutex};
        for (auto &entry : mappingEvidence) {
            if (entry.active && entry.addressSpace == addressSpace &&
                Overlaps(entry.gpuBase, entry.bytes, gpuBase, bytes))
                entry.active = false;
        }
    }

    inline std::optional<MappingEvidence> FindMapping(const void *addressSpace,
                                                       std::uint64_t gpuIova,
                                                       std::uintptr_t observedHostAddress) {
        if (!addressSpace || !observedHostAddress)
            return {};
        std::scoped_lock lock{mappingEvidenceMutex};
        std::optional<MappingEvidence> result;
        for (const auto &entry : mappingEvidence) {
            if (!entry.active || entry.addressSpace != addressSpace ||
                gpuIova < entry.gpuBase || gpuIova - entry.gpuBase >= entry.bytes)
                continue;
            const auto offset{gpuIova - entry.gpuBase};
            if (offset > UINTPTR_MAX - entry.hostBase ||
                entry.hostBase + static_cast<std::uintptr_t>(offset) != observedHostAddress)
                continue;
            if (!result || entry.generation > result->generation)
                result = entry;
        }
        return result;
    }

    // Candidate memory poison pattern observed in the first Kirby log.
    // A matching constant is evidence of a POSSIBLE source, not a proof that
    // a DMA engine produced the TIC: it may be copied or overwritten later.
    inline constexpr std::uint32_t SuspectDword{0x7FBFFFFF};
    inline std::atomic<std::uint32_t> emittedWriterEvents{0};
    inline constexpr std::uint32_t MaxWriterEvents{48};

    inline bool ShouldEmitWriter() {
        return emittedWriterEvents.fetch_add(1, std::memory_order_relaxed) < MaxWriterEvents;
    }

    inline std::atomic<std::uint32_t> emittedEvents{0};
    inline thread_local std::array<std::uint64_t, 64> recentFingerprints{};
    inline thread_local std::uint32_t nextFingerprint{};
    inline constexpr std::uint32_t MaxEmittedEvents{96};

    inline std::uint64_t Fingerprint(std::uint64_t poolBase, std::uint32_t index,
                                     const std::array<std::uint32_t, 8> &words,
                                     std::uint64_t shaderHash, std::uint32_t site) {
        std::uint64_t hash{14695981039346656037ULL};
        auto add{[&](std::uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }};
        add(poolBase);
        add(index);
        for (auto word : words)
            add(word);
        add(shaderHash);
        add(site);
        return hash ? hash : 1;
    }

    inline bool ShouldEmit(std::uint64_t fingerprint) {
        if (!fingerprint)
            fingerprint = 1;
        for (auto seen : recentFingerprints)
            if (seen == fingerprint)
                return false;

        recentFingerprints[nextFingerprint++ % recentFingerprints.size()] = fingerprint;
        return emittedEvents.fetch_add(1, std::memory_order_relaxed) < MaxEmittedEvents;
    }
}
