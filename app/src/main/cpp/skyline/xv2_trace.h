// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace skyline::diagnostics::xv2 {
    inline std::atomic<std::uint64_t> postCloseEpoch{};
    inline std::atomic<std::uint64_t> lastClosedStreamId{};
    inline std::atomic<std::uint32_t> postClosePipelineSequence{};
    inline std::atomic<std::uint32_t> postCloseDrawSequence{};
    inline std::atomic<std::uint32_t> postCloseDirtyReadSequence{};
    inline std::atomic<std::uint32_t> postCloseGpuSubmitSequence{};
    inline std::atomic<std::uint32_t> postCloseGpfifoSequence{};
    inline std::atomic<std::uint32_t> postCloseMaxwellMethodSequence{};
    inline std::atomic<std::uint32_t> postCloseBufferQueueSequence{};
    inline std::atomic<std::uint32_t> postClosePresentSequence{};
    inline std::atomic<std::uint32_t> postCloseSvcWaitSequence{};
    inline std::atomic<std::uint32_t> postClosePullerSequence{};
    inline std::atomic<std::uint32_t> postCloseIpcSequence{};
    inline std::atomic<std::uint32_t> postCloseNvdrvSequence{};

    inline std::uint64_t MarkStreamClosed(std::uint64_t streamId) {
        lastClosedStreamId.store(streamId, std::memory_order_relaxed);
        postClosePipelineSequence.store(0, std::memory_order_relaxed);
        postCloseDrawSequence.store(0, std::memory_order_relaxed);
        postCloseDirtyReadSequence.store(0, std::memory_order_relaxed);
        postCloseGpuSubmitSequence.store(0, std::memory_order_relaxed);
        postCloseGpfifoSequence.store(0, std::memory_order_relaxed);
        postCloseMaxwellMethodSequence.store(0, std::memory_order_relaxed);
        postCloseBufferQueueSequence.store(0, std::memory_order_relaxed);
        postClosePresentSequence.store(0, std::memory_order_relaxed);
        postCloseSvcWaitSequence.store(0, std::memory_order_relaxed);
        postClosePullerSequence.store(0, std::memory_order_relaxed);
        postCloseIpcSequence.store(0, std::memory_order_relaxed);
        postCloseNvdrvSequence.store(0, std::memory_order_relaxed);
        return postCloseEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    inline bool PostCloseActive() {
        return postCloseEpoch.load(std::memory_order_relaxed) != 0;
    }

    inline std::uint64_t Epoch() {
        return postCloseEpoch.load(std::memory_order_relaxed);
    }

    inline std::uint64_t LastClosedStreamId() {
        return lastClosedStreamId.load(std::memory_order_relaxed);
    }

    inline std::uint32_t NextPipelineSequence() {
        return postClosePipelineSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextDrawSequence() {
        return postCloseDrawSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextDirtyReadSequence() {
        return postCloseDirtyReadSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextGpuSubmitSequence() {
        return postCloseGpuSubmitSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextGpfifoSequence() {
        return postCloseGpfifoSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextMaxwellMethodSequence() {
        return postCloseMaxwellMethodSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextBufferQueueSequence() {
        return postCloseBufferQueueSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextPresentSequence() {
        return postClosePresentSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextSvcWaitSequence() {
        return postCloseSvcWaitSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextPullerSequence() {
        return postClosePullerSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextIpcSequence() {
        return postCloseIpcSequence.fetch_add(1, std::memory_order_relaxed);
    }

    inline std::uint32_t NextNvdrvSequence() {
        return postCloseNvdrvSequence.fetch_add(1, std::memory_order_relaxed);
    }
}
