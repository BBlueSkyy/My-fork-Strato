// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>
#include <soc/host1x/frame_queue.h>
#include "registers.h"

struct AVCodecContext;
struct AVPacket;

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief A software decoder for a single NVDEC codec instance backed by FFmpeg's libavcodec
     */
    class FfmpegDecoder {
      private:
        AVCodecContext *context{};
        AVPacket *packet{};

      public:
        ~FfmpegDecoder();

        /**
         * @brief Creates and opens a decoder context for the supplied codec
         * @return If the decoder was successfully initialised
         */
        bool Initialize(CodecId codecId);

        /**
         * @brief Submits a composed bitstream packet to the decoder
         * @param surfaceKey An opaque key identifying the output surface of this operation, carried onto the resulting frame's PTS across any decoder reordering
         * @return If the packet was accepted
         */
        bool SendPacket(span<const u8> data, u64 surfaceKey);

        /**
         * @brief Retrieves the next decoded frame from the decoder
         * @return The decoded frame with its submission's surface key in the PTS, or an empty pointer when no frame is available yet
         */
        AVFramePtr ReceiveFrame();
    };
}
