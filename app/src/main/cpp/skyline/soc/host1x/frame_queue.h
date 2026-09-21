// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <common.h>

struct AVFrame;

namespace skyline::soc::host1x {
    using AVFramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame *)>; //!< A decoded FFmpeg frame carrying its own deleter so this header doesn't depend on libavutil

    /**
     * @brief Holds frames decoded by NVDEC until they are consumed by VIC for surface conversion
     * @note This is thread-safe as NVDEC and VIC execute on separate channel FIFO threads
     */
    class FrameQueue {
      private:
        std::mutex mutex; //!< Synchronises access to the frame list across channel threads
        std::deque<std::pair<u64, AVFramePtr>> presentationFrames; //!< Frames in the presentation order returned by FFmpeg, retaining the submission luma IOVA as metadata
        constexpr static size_t MaxQueueSize{32}; //!< Cap on retained presentation frames so an unconsumed stream cannot accumulate unboundedly

      public:
        /**
         * @brief Appends a decoded frame in the presentation order produced by FFmpeg
         * @param lumaIova The submission surface associated with the frame, retained for diagnostics
         * @note Repeated IOVAs are intentionally retained: surface reuse must not drop intermediate presentation frames
         */
        void PushPresentationFrame(u64 lumaIova, AVFramePtr frame);

        /**
         * @brief Removes and returns the next frame in presentation order without blocking
         * @param requestedLumaIova The VIC input surface for diagnostics; presentation order is authoritative
         * @return The next presentation frame, or an empty pointer when no frame is available
         */
        AVFramePtr PopPresentationFrame(u64 requestedLumaIova);
    };
}
