// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace skyline::diagnostics::xv2 {
    inline std::atomic<std::uint64_t> postCloseEpoch{};

    inline void MarkStreamClosed() {
        postCloseEpoch.fetch_add(1, std::memory_order_relaxed);
    }

    inline bool PostCloseActive() {
        return postCloseEpoch.load(std::memory_order_relaxed) != 0;
    }

    inline std::uint64_t Epoch() {
        return postCloseEpoch.load(std::memory_order_relaxed);
    }
}
