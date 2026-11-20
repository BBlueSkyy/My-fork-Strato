// SPDX-License-Identifier: MPL-2.0
// Diagnostic-only telemetry for PR #469. Guard inclusion with STRATO_OCEAN_VIS_MODE == 1.
//
// Aggregates `Textures::GetTexture` -> NullTextureReason::Unmapped failures by descriptor
// identity so the TICs behind the missing distant textures can be named. It never
// changes what GetTexture returns. Per lookup it costs a hash probe under a mutex; the
// GMMU range analysis only runs the first time a descriptor is seen and then once every
// ReanalyzeInterval hits, to catch mappings that change during a session.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>
#include <logger/logger.h>

// The tag stays a literal inside the format string so each line is greppable and testable by prefix
#define STRATO_OCEAN_LOG_ENTRY(TAG, e, rank) \
do { \
    const auto &d{(e).desc}; \
    LOGI(TAG " rank={} index={} iova=0x{:X} backing=0x{:X} backingValid={} fmt=0x{:X} {}x{}x{} type={} header={} sparse={} mips={}/base{}/count{} layer={} backingLayers={}/{} tile={}:{} shaderType={} hits={} lookups={}-{}", \
         rank, d.index, d.iova, d.backingIova, d.backingIovaValid, d.formatWord, \
         d.width, d.height, d.depth, d.textureType, d.headerType, d.isSparse, d.totalMips, d.viewMipBase, d.viewMipCount, \
         d.baseLayer, d.minBackingLayers, d.fullBackingLayers, d.tileMode, d.blockHeight, d.shaderType, \
         (e).hits, (e).firstLookup, (e).lastLookup); \
    LOGI(TAG " rank={} index={} min: req={} valid={} invalid={} firstInvalid={} segs={}/{} pages4k={}/{} base={} | full: req={} valid={} invalid={} firstInvalid={} segs={}/{} pages4k={}/{} base={}", \
         rank, d.index, \
         (e).minimum.requestedBytes, (e).minimum.validBytes, (e).minimum.invalidBytes, (e).minimum.firstInvalidOffset, \
         (e).minimum.invalidSegments, (e).minimum.segments, (e).minimum.validPages4k, (e).minimum.invalidPages4k, (e).minimum.baseMapped, \
         (e).full.requestedBytes, (e).full.validBytes, (e).full.invalidBytes, (e).full.firstInvalidOffset, \
         (e).full.invalidSegments, (e).full.segments, (e).full.validPages4k, (e).full.invalidPages4k, (e).full.baseMapped); \
    LOGI(TAG " rank={} index={} changes={} resolvedBuilds={} resolvedAt={} reboundBuilds={} reboundIova=0x{:X} shader0=0x{:016X}@{} shader1=0x{:016X}@{} shaders={}+{}", \
         rank, d.index, (e).mappingChanges, (e).resolvedBuilds, (e).resolvedAtLookup, (e).reboundBuilds, (e).reboundIova, \
         (e).shaders[0], (e).stages[0], (e).shaders[1], (e).stages[1], (e).shaderCount, (e).shaderOverflow); \
} while (false)

namespace skyline::gpu::diagnostics::oceanhorn {
    /**
     * @brief Where a requested backing range stops being mapped in GMMU
     * @note Built from the segments TranslateRange returned: an unmapped segment is a null span of the same size
     */
    struct MappingAnalysis {
        static constexpr uint64_t NoInvalid{std::numeric_limits<uint64_t>::max()};
        uint64_t requestedBytes{};
        uint64_t validBytes{}, invalidBytes{};
        uint64_t firstInvalidOffset{NoInvalid}; //!< Byte offset inside the requested range of the first unmapped segment
        uint32_t segments{}, invalidSegments{};
        uint32_t validPages4k{}, invalidPages4k{};
        bool baseMapped{}; //!< The very first segment (the TIC's own base address) is mapped

        bool Complete() const { return requestedBytes && !invalidSegments; }
        bool operator==(const MappingAnalysis &) const = default;
    };

    /**
     * @param segments (valid, size) pairs in address order, exactly as translated
     */
    inline MappingAnalysis AnalyzeSegments(const std::vector<std::pair<bool, uint64_t>> &segments) {
        constexpr uint64_t PageSize{0x1000};
        MappingAnalysis a{};
        a.segments = static_cast<uint32_t>(segments.size());
        for (const auto &[valid, size] : segments) {
            const auto pages{static_cast<uint32_t>((size + PageSize - 1) / PageSize)};
            if (valid) {
                a.validBytes += size;
                a.validPages4k += pages;
            } else {
                if (a.firstInvalidOffset == MappingAnalysis::NoInvalid)
                    a.firstInvalidOffset = a.requestedBytes;
                a.invalidBytes += size;
                a.invalidPages4k += pages;
                ++a.invalidSegments;
            }
            a.requestedBytes += size;
        }
        a.baseMapped = !segments.empty() && segments.front().first;
        return a;
    }

    /**
     * @brief Identity of a sampled TIC as GetTexture decoded it
     */
    struct TicDescriptor {
        uint32_t index{};
        uint64_t iova{};        //!< Address stored in the TIC
        uint64_t backingIova{}; //!< Address after the baseArrayLayer rewind
        bool backingIovaValid{true};
        uint32_t formatWord{};
        uint32_t width{}, height{}, depth{};
        uint32_t textureType{}, headerType{};
        bool isSparse{};
        uint32_t totalMips{}, viewMipBase{}, viewMipCount{};
        uint32_t baseLayer{}, minBackingLayers{}, fullBackingLayers{};
        uint32_t tileMode{}, blockHeight{};
        uint32_t shaderType{};
        uint64_t shaderKey{};   //!< Hash of the pipeline's shader hashes, 0 if unknown
        uint32_t stage{};       //!< Vulkan stage bit of the binding that asked for it

        // Same descriptor == same slot, address, layout and sampled dimensionality. The
        // shader/stage are deliberately not part of it, they are collected per entry.
        bool SameDescriptor(const TicDescriptor &o) const {
            return index == o.index && iova == o.iova && formatWord == o.formatWord &&
                width == o.width && height == o.height && depth == o.depth &&
                isSparse == o.isSparse &&
                totalMips == o.totalMips && viewMipBase == o.viewMipBase && viewMipCount == o.viewMipCount &&
                baseLayer == o.baseLayer && shaderType == o.shaderType;
        }
    };

    /**
     * @brief Context the descriptor binder publishes for the lookup that follows on the same thread
     */
    struct BindingContext {
        uint64_t shaderKey{};
        uint32_t stage{};
    };

    inline thread_local BindingContext currentBinding{};

    /**
     * @brief The same FNV-1a fold over a pipeline's shader hashes that RecordDrawDiagnostic reports as OCEAN-SHADERS keys
     */
    template<typename Hashes>
    uint64_t HashShaderStages(const Hashes &hashes) {
        uint64_t key{0xcbf29ce484222325ULL};
        for (auto hash : hashes) {
            key ^= hash;
            key *= 0x100000001b3ULL;
        }
        return key;
    }

    class UnmappedTicCapture {
      public:
        static constexpr size_t Capacity{256};
        static constexpr uint64_t ReanalyzeInterval{1u << 14};
        static constexpr size_t MaxShadersPerEntry{4};
        static constexpr uint32_t MaxFirstSeenLogs{64};

        struct Entry {
            TicDescriptor desc{};
            MappingAnalysis minimum{}, full{}; //!< Latest analysis of the minimum and the pool-wide backing extent
            uint64_t requestedMinimum{}, requestedFull{};
            uint64_t hits{}, hitsAtLastReport{};
            uint64_t firstLookup{}, lastLookup{};
            uint32_t mappingChanges{};  //!< Re-analysis disagreed with an earlier analysis
            uint32_t resolvedBuilds{};  //!< Same descriptor later produced a guest-backed texture
            uint64_t resolvedAtLookup{};
            uint32_t reboundBuilds{};   //!< Same TIC index later built from a different IOVA
            uint64_t reboundIova{};
            std::array<uint64_t, MaxShadersPerEntry> shaders{};
            uint32_t shaderCount{}, shaderOverflow{};
            std::array<uint32_t, MaxShadersPerEntry> stages{};
            bool used{};
        };

        /**
         * @brief Records one Unmapped fallback
         * @param analyze Called only when the descriptor is new or due for re-analysis, returns {minimum, full} analyses
         */
        template<typename Analyze>
        void Observe(const TicDescriptor &desc, uint64_t requestedMinimum, uint64_t requestedFull, Analyze &&analyze) {
            std::scoped_lock lock{mutex};
            ++lookups;
            Entry *entry{Find(desc)};
            bool fresh{};
            if (!entry) {
                if (used == entries.size()) {
                    ++overflowLookups;
                    return;
                }
                entry = &entries[used++];
                entry->used = true;
                entry->desc = desc;
                entry->firstLookup = lookups;
                fresh = true;
            }
            ++entry->hits;
            entry->lastLookup = lookups;
            entry->requestedMinimum = requestedMinimum;
            entry->requestedFull = requestedFull;
            AddShader(*entry, desc.shaderKey, desc.stage);

            if (fresh || entry->hits % ReanalyzeInterval == 0) {
                const auto [minimum, full]{analyze()};
                if (!fresh && (!(minimum == entry->minimum) || !(full == entry->full)))
                    ++entry->mappingChanges;
                entry->minimum = minimum;
                entry->full = full;
                if (fresh && firstSeenLogged < MaxFirstSeenLogs) {
                    ++firstSeenLogged;
                    STRATO_OCEAN_LOG_ENTRY("[OCEAN-UNMAPPED-NEW]", *entry, used);
                }
            }
        }

        /**
         * @brief A sampled TIC was just materialised into a guest-backed texture
         * @note Called on the build path only, never for cache hits
         */
        void NoteBuilt(const TicDescriptor &desc) {
            std::scoped_lock lock{mutex};
            for (size_t i{}; i < used; ++i) {
                auto &e{entries[i]};
                if (e.desc.index != desc.index)
                    continue;
                if (e.desc.SameDescriptor(desc)) {
                    ++e.resolvedBuilds;
                    e.resolvedAtLookup = lookups;
                } else if (e.desc.iova != desc.iova) {
                    ++e.reboundBuilds;
                    e.reboundIova = desc.iova;
                }
            }
        }

        /**
         * @brief Emits a bounded report: a summary line and the busiest descriptors
         */
        void Report(uint64_t reportSerial) {
            if (reportSerial > 8 && reportSerial % 4)
                return;
            std::scoped_lock lock{mutex};
            uint64_t totalHits{}, deltaHits{}, sparseHits{};
            uint32_t unresolved{}, partial{}, baseUnmapped{}, changed{}, sparseDistinct{};
            std::array<const Entry *, 8> top{};
            for (size_t i{}; i < used; ++i) {
                auto &e{entries[i]};
                const auto delta{e.hits - e.hitsAtLastReport};
                totalHits += e.hits;
                deltaHits += delta;
                if (e.desc.isSparse) {
                    ++sparseDistinct;
                    sparseHits += e.hits;
                }
                if (!e.resolvedBuilds) ++unresolved;
                if (e.minimum.baseMapped && !e.minimum.Complete()) ++partial;
                if (!e.minimum.baseMapped) ++baseUnmapped;
                if (e.mappingChanges) ++changed;
                for (size_t t{}; t < top.size(); ++t) {
                    if (!top[t] || delta > top[t]->hits - top[t]->hitsAtLastReport) {
                        for (size_t j{top.size() - 1}; j > t; --j)
                            top[j] = top[j - 1];
                        top[t] = &e;
                        break;
                    }
                }
            }
            LOGI("[OCEAN-UNMAPPED] seq={} distinct={} hits={} windowHits={} sparseDistinct={} sparseHits={} neverBuilt={} baseMapped_partial={} baseUnmapped={} mappingChanged={} tableFull={}",
                 reportSerial, used, totalHits, deltaHits, sparseDistinct, sparseHits, unresolved, partial, baseUnmapped, changed, overflowLookups);
            for (size_t t{}; t < top.size() && top[t]; ++t)
                STRATO_OCEAN_LOG_ENTRY("[OCEAN-UNMAPPED-TOP]", *top[t], t);
            for (size_t i{}; i < used; ++i)
                entries[i].hitsAtLastReport = entries[i].hits;
        }

        size_t Size() const { return used; }
        const Entry &At(size_t i) const { return entries[i]; }
        uint64_t Overflow() const { return overflowLookups; }
        uint64_t Lookups() const { return lookups; }

        static UnmappedTicCapture &Instance() {
            static UnmappedTicCapture instance;
            return instance;
        }

      private:
        std::mutex mutex;
        std::array<Entry, Capacity> entries{};
        size_t used{};
        uint64_t lookups{}, overflowLookups{};
        uint32_t firstSeenLogged{};

        Entry *Find(const TicDescriptor &desc) {
            for (size_t i{}; i < used; ++i)
                if (entries[i].desc.SameDescriptor(desc))
                    return &entries[i];
            return nullptr;
        }

        static void AddShader(Entry &e, uint64_t shaderKey, uint32_t stage) {
            for (uint32_t i{}; i < e.shaderCount; ++i)
                if (e.shaders[i] == shaderKey && e.stages[i] == stage)
                    return;
            if (e.shaderCount < MaxShadersPerEntry) {
                e.shaders[e.shaderCount] = shaderKey;
                e.stages[e.shaderCount] = stage;
                ++e.shaderCount;
            } else {
                ++e.shaderOverflow;
            }
        }
    };
}

#undef STRATO_OCEAN_LOG_ENTRY
