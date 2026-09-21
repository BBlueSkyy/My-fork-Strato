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
            LOGI("[NVDEC-LIFE] Presentation queue empty, queue: {}, VIC luma IOVA: 0x{:X}",
                 fmt::ptr(this), requestedLumaIova);
            return AVFramePtr{nullptr, nullptr};
        }

        auto &[submittedLumaIova, queuedFrame]{presentationFrames.front()};
        LOGD("Presentation frame dequeue, queue: {}, VIC luma: 0x{:X}, submitted luma: 0x{:X}, queued frames: {}",
             fmt::ptr(this), requestedLumaIova, submittedLumaIova, presentationFrames.size());

        auto frame{std::move(queuedFrame)};
        presentationFrames.pop_front();
        return frame;
    }
}
