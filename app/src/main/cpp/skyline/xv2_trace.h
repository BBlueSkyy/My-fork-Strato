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

    inline std::uint64_t MarkStreamClosed(std::uint64_t streamId) {
        lastClosedStreamId.store(streamId, std::memory_order_relaxed);
        postClosePipelineSequence.store(0, std::memory_order_relaxed);
        postCloseDrawSequence.store(0, std::memory_order_relaxed);
        postCloseDirtyReadSequence.store(0, std::memory_order_relaxed);
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
}
