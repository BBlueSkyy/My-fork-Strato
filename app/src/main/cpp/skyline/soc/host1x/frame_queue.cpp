// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include "frame_queue.h"

namespace skyline::soc::host1x {
    void FrameQueue::PushPresentationFrame(u64 lumaIova, AVFramePtr frame) {
        std::scoped_lock lock(mutex);

        if (presentationFrames.size() >= MaxQueueSize) {
            LOGW("Presentation frame queue overflow, dropping oldest frame with luma IOVA: 0x{:X}",
                 presentationFrames.front().first);
            presentationFrames.pop_front();
        }

        // FFmpeg's receive order is the presentation order for this software decoder path.
        // Do not replace an existing entry when the guest reuses the same surface: each
        // decoded frame is a distinct presentation event and must remain in sequence.
        presentationFrames.emplace_back(lumaIova, std::move(frame));
    }

    AVFramePtr FrameQueue::PopPresentationFrame(u64 requestedLumaIova) {
        std::scoped_lock lock(mutex);

        if (presentationFrames.empty()) {
            LOGD("Presentation frame queue empty for VIC luma IOVA: 0x{:X}", requestedLumaIova);
            return AVFramePtr{nullptr, nullptr};
        }

        auto &[submittedLumaIova, queuedFrame]{presentationFrames.front()};
        LOGD("Presentation frame dequeue, VIC luma: 0x{:X}, submitted luma: 0x{:X}, queued frames: {}",
             requestedLumaIova, submittedLumaIova, presentationFrames.size());

        auto frame{std::move(queuedFrame)};
        presentationFrames.pop_front();
        return frame;
    }
}
