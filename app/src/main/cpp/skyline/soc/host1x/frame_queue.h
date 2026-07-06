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
        std::deque<std::pair<u64, AVFramePtr>> frames; //!< Decoded frames in decode order alongside the SMMU IOVA of the output luma plane NVDEC was programmed with
        constexpr static size_t MaxQueueSize{32}; //!< Cap on retained frames so frames that are never consumed don't accumulate unboundedly

      public:
        /**
         * @brief Stores a decoded frame under the IOVA of the luma surface it was decoded into
         * @note If a frame is already stored under the same IOVA it is replaced as the guest has reused the surface
         */
        void PushFrame(u64 lumaIova, AVFramePtr frame);

        /**
         * @brief Removes and returns the frame stored under the supplied luma IOVA
         * @return The stored frame, the oldest frame as a fallback when no key matches, or an empty pointer when the queue is empty
         */
        AVFramePtr PopFrame(u64 lumaIova);
    };
}
