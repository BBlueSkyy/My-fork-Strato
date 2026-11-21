// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>

namespace skyline::diagnostics {
    struct Oh2PerfCounters {
        std::atomic<uint64_t> lastReportNs{};

        std::atomic<uint64_t> sparseReadCalls{};
        std::atomic<uint64_t> sparseReadNs{};
        std::atomic<uint64_t> sparseReadMaxNs{};
        std::atomic<uint64_t> sparseReadAttempts{};
        std::atomic<uint64_t> sparseStable{};
        std::atomic<uint64_t> sparseUnstable{};
        std::atomic<uint64_t> sparseUnmapped{};
        std::atomic<uint64_t> sparseBackingBytes{};
        std::atomic<uint64_t> sparseResidentBytes{};
        std::atomic<uint64_t> sparseSegments{};
        std::atomic<uint64_t> sparseSegmentsMax{};

        std::atomic<uint64_t> sparseMirrorMaps{};
        std::atomic<uint64_t> sparseMirrorMappedBytes{};
        std::atomic<uint64_t> sparseMirrorMapNs{};
        std::atomic<uint64_t> sparseMirrorCopyNs{};
        std::atomic<uint64_t> sparseMirrorUnmapNs{};
        std::atomic<uint64_t> sparseZeroNs{};

        std::atomic<uint64_t> sparseRefreshCalls{};
        std::atomic<uint64_t> sparseRefreshChanged{};
        std::atomic<uint64_t> sparseRefreshDeleteTrap{};
        std::atomic<uint64_t> sparseRefreshCreateTrap{};
        std::atomic<uint64_t> sparseRefreshTrapRegions{};
        std::atomic<uint64_t> sparseRefreshResidentSpans{};
        std::atomic<uint64_t> sparseRefreshNs{};
        std::atomic<uint64_t> sparseRefreshMaxNs{};
        std::atomic<uint64_t> sparseGenerationProbeNs{};
        std::atomic<uint64_t> sparseMappingScanNs{};
        std::atomic<uint64_t> sparseFenceWaitCalls{};
        std::atomic<uint64_t> sparseFenceWaitNs{};
        std::atomic<uint64_t> sparseFenceWaitMaxNs{};

        std::atomic<uint64_t> sparseHostSyncCalls{};
        std::atomic<uint64_t> sparseHostSyncNs{};
        std::atomic<uint64_t> sparseDeswizzleNs{};
        std::atomic<uint64_t> sparseStagingAllocNs{};
        std::atomic<uint64_t> sparseUploadRecordNs{};
        std::atomic<uint64_t> sparseGpuUploadCalls{};
        std::atomic<uint64_t> sparseGpuUploadBytes{};
        std::atomic<uint64_t> sparseGpuUploadRegions{};

        std::atomic<uint64_t> gpuQueueSubmitCalls{};
        std::atomic<uint64_t> gpuQueueSubmitNs{};
        std::atomic<uint64_t> gpuFenceWaitCalls{};
        std::atomic<uint64_t> gpuFenceWaitNs{};
        std::atomic<uint64_t> gpuFenceWaitMaxNs{};

        std::atomic<uint64_t> trapRegionsCalls{};
        std::atomic<uint64_t> trapRemoveCalls{};
        std::atomic<uint64_t> trapDeleteCalls{};
        std::atomic<uint64_t> trapReprotectCalls{};
        std::atomic<uint64_t> trapMprotectCalls{};
        std::atomic<uint64_t> trapMprotectNs{};
    };

    inline Oh2PerfCounters oh2Perf{};

    inline uint64_t PerfNowNs() noexcept {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline void AtomicMax(std::atomic<uint64_t> &value, uint64_t candidate) noexcept {
        auto current{value.load(std::memory_order_relaxed)};
        while (current < candidate &&
               !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }
}
