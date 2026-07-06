// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include <algorithm>
#include "frame_queue.h"

namespace skyline::soc::host1x {
    void FrameQueue::PushFrame(u64 lumaIova, AVFramePtr frame) {
        std::scoped_lock lock(mutex);

        auto it{std::find_if(frames.begin(), frames.end(), [&](const auto &entry) { return entry.first == lumaIova; })};
        if (it != frames.end()) {
            // The guest has reused the surface before consuming the previous frame, replace it
            it->second = std::move(frame);
            return;
        }

        if (frames.size() >= MaxQueueSize) {
            LOGW("Frame queue overflow, dropping frame with luma IOVA: 0x{:X}", frames.front().first);
            frames.pop_front();
        }

        frames.emplace_back(lumaIova, std::move(frame));
    }

    AVFramePtr FrameQueue::PopFrame(u64 lumaIova) {
        std::scoped_lock lock(mutex);

        if (frames.empty())
            return AVFramePtr{nullptr, nullptr};

        auto it{std::find_if(frames.begin(), frames.end(), [&](const auto &entry) { return entry.first == lumaIova; })};
        if (it == frames.end()) {
            // Fall back to the oldest frame, a decoder that delays output shifts frames onto later
            // surface keys so the oldest queued frame is the best match for the requested surface
            LOGD("Frame queue miss for luma IOVA: 0x{:X}, falling back to oldest frame with: 0x{:X}", lumaIova, frames.front().first);
            it = frames.begin();
        }

        auto frame{std::move(it->second)};
        frames.erase(it);
        return frame;
    }
}
