// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <common.h>

struct AVFrame;

namespace skyline::soc::host1x {
    using AVFramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame *)>;

    /**
     * @brief Holds frames decoded by NVDEC until VIC consumes them, preserving presentation order independently for each nvhost stream
     */
    class FrameQueue {
      private:
        using PresentationQueue = std::deque<std::pair<u64, AVFramePtr>>;

        std::mutex mutex;
        std::unordered_map<u64, PresentationQueue> presentationStreams;
        constexpr static size_t MaxQueueSize{32};

      public:
        void OpenStream(u64 streamId);

        void CloseStream(u64 streamId);

        /**
         * @brief Appends one visible FFmpeg output frame to the presentation queue for a specific NVDEC stream
         * @note Repeated IOVAs are retained because every decoded frame is a distinct presentation event
         */
        void PushPresentationFrame(u64 streamId, u64 lumaIova, AVFramePtr frame);

        /**
         * @brief Finds the NVDEC stream owning the requested luma surface and consumes its next presentation frame
         * @note The luma IOVA selects the stream, not the frame within that stream. Once selected, FIFO presentation order remains authoritative.
         */
        AVFramePtr PopPresentationFrame(u64 requestedLumaIova);
    };
}
