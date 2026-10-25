// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace skyline::nce {
    struct SvcCallsite {
        std::uint64_t pc{}, lr{}, sp{}, fp{};
    };

    template<class Reader>
    std::vector<std::uint64_t> WalkGuestFrames(std::uint64_t fp, Reader &&read) {
        std::vector<std::uint64_t> frames;
        // Limit diagnostic output, never guest execution. Reject cycles and
        // unreadable frames rather than dereferencing a guest FP on the host.
        for (std::size_t i{}; fp && i < 32; ++i) {
            std::array<std::uint64_t, 2> frame{};
            if (fp % 16 || !read(fp, frame.data(), sizeof(frame))) break;
            if (frame[1]) frames.push_back(frame[1]);
            if (frame[0] <= fp) break;
            fp = frame[0];
        }
        return frames;
    }
}
