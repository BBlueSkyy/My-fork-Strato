// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include <algorithm>
#include "frame_queue.h"
#include "video_diag.h"

namespace skyline::soc::host1x {
    void FrameQueue::OpenStream(u64 streamId) {
        std::scoped_lock lock(mutex);
        presentationStreams.try_emplace(streamId);
    }

    void FrameQueue::CloseStream(u64 streamId) {
        std::scoped_lock lock(mutex);
        presentationStreams.erase(streamId);
    }

    void FrameQueue::PushPresentationFrame(u64 streamId, u64 lumaIova, AVFramePtr frame) {
        std::scoped_lock lock(mutex);

        auto &frames{presentationStreams[streamId]};
        if (frames.size() >= MaxPresentationQueueSize) {
            LOGW("Presentation frame queue overflow for stream {}, dropping oldest frame with luma IOVA: 0x{:X}",
                 streamId, frames.front().first);
            frames.pop_front();
        }

        frames.emplace_back(lumaIova, std::move(frame));
        static std::atomic<u64> pushCount{0};
        const u64 sequence{++pushCount};
        if (video_diag::Sample(sequence))
            LOGI("[VideoDiag] queue-push seq={} stream={} luma=0x{:X} depth={}",
                 sequence, streamId, lumaIova, frames.size());
    }

    AVFramePtr FrameQueue::PopPresentationFrame(u64 requestedLumaIova) {
        static std::atomic<u64> popCount{0};
        const u64 sequence{++popCount};
        const bool trace{video_diag::Sample(sequence)};
        std::scoped_lock lock(mutex);

        PresentationQueue *selected{};
        u64 selectedStream{};
        PresentationQueue *onlyNonEmpty{};
        u64 onlyNonEmptyStream{};
        size_t nonEmptyStreams{};

        for (auto &[streamId, frames] : presentationStreams) {
            if (frames.empty())
                continue;

            nonEmptyStreams++;
            onlyNonEmpty = &frames;
            onlyNonEmptyStream = streamId;

            if (std::any_of(frames.begin(), frames.end(),
                            [&](const auto &entry) { return entry.first == requestedLumaIova; })) {
                selected = &frames;
                selectedStream = streamId;
                break;
            }
        }

        // Preserve the known-good single-stream presentation semantics used before stream
        // isolation. The luma IOVA is only required to disambiguate when multiple NVDEC streams
        // can actually provide frames at the same time.
        if (!selected && nonEmptyStreams == 1) {
            selected = onlyNonEmpty;
            selectedStream = onlyNonEmptyStream;
        }

        if (!selected) {
            LOGD("No unambiguous presentation stream for VIC luma IOVA: 0x{:X}, active streams: {}",
                 requestedLumaIova, nonEmptyStreams);
            if (trace)
                LOGI("[VideoDiag] queue-miss seq={} requested=0x{:X} nonemptyStreams={}",
                     sequence, requestedLumaIova, nonEmptyStreams);
            return AVFramePtr{nullptr, nullptr};
        }

        auto &[submittedLumaIova, queuedFrame]{selected->front()};
        LOGD("Presentation frame dequeue, stream: {}, VIC luma: 0x{:X}, submitted luma: 0x{:X}, queued frames: {}",
             selectedStream, requestedLumaIova, submittedLumaIova, selected->size());
        if (trace)
            LOGI("[VideoDiag] queue-pop seq={} stream={} requested=0x{:X} submitted=0x{:X} depth={}",
                 sequence, selectedStream, requestedLumaIova, submittedLumaIova, selected->size());

        auto frame{std::move(queuedFrame)};
        selected->pop_front();
        return frame;
    }
}
