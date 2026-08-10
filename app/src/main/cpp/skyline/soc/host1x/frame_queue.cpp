// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include <algorithm>
#include <chrono>
#include "frame_queue.h"

namespace skyline::soc::host1x {
    void FrameQueue::PushFrame(u64 lumaIova, AVFramePtr frame) {
        {
            std::scoped_lock lock(mutex);

            auto it{std::find_if(frames.begin(), frames.end(), [&](const auto &entry) { return entry.first == lumaIova; })};
            if (it != frames.end()) {
                // The guest has reused the surface before consuming the previous frame, replace it
                it->second = std::move(frame);
            } else {
                if (frames.size() >= MaxQueueSize) {
                    LOGW("Frame queue overflow, dropping frame with luma IOVA: 0x{:X}", frames.front().first);
                    frames.pop_front();
                }

                frames.emplace_back(lumaIova, std::move(frame));
            }
        }

        frameCondition.notify_all();
    }

    AVFramePtr FrameQueue::PopFrame(u64 lumaIova) {
        constexpr std::chrono::milliseconds FrameWaitTimeout{40}; //!< Over two 60Hz frames, enough for a reordering decoder to emit a held frame on the next decode operation

        std::unique_lock lock(mutex);

        auto findFrame{[&] { return std::find_if(frames.begin(), frames.end(), [&](const auto &entry) { return entry.first == lumaIova; }); }};

        auto it{findFrame()};
        if (it == frames.end()) {
            // The frame may still be held inside the decoder's reorder buffer, give it a chance to arrive
            frameCondition.wait_for(lock, FrameWaitTimeout, [&] { return findFrame() != frames.end(); });
            it = findFrame();
        }

        if (it == frames.end()) {
            if (frames.empty())
                return AVFramePtr{nullptr, nullptr};

            // Fall back to the oldest frame so a mismatch degrades to a stale frame rather than no frame
            LOGD("Frame queue miss for luma IOVA: 0x{:X}, falling back to oldest frame with: 0x{:X}", lumaIova, frames.front().first);
            it = frames.begin();
        }

        auto frame{std::move(it->second)};
        frames.erase(it);
        return frame;
    }
}
