// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>
#include <map>
#include <soc/host1x/classes/nvdec/output_surface.h>
#include <soc/host1x/classes/nvdec/ffmpeg_decoder.h>
#include <soc/host1x/classes/nvdec/registers.h>

namespace skyline::soc::host1x::nvdec {
    /**
     * @brief The base of all NVDEC codec implementations, decoding is driven by re-composing a standard bitstream from the guest register state and feeding it through FFmpeg
     */
    class Codec {
      protected:
        constexpr static u32 MaxBitstreamSize{1 << 24}; //!< 16MiB, sanity cap on guest-supplied bitstream sizes

        const DeviceState &state;
        const Registers &registers;
        FfmpegDecoder decoder;
        struct Submission {
            u64 surfaceKey;
            OutputSurface output;
            bool hidden;
            bool materialized{};
        };
        u64 nextSubmission{1};
        // H.264 has at most 16 reference frames (32 field pictures). Keep
        // twice that window for decoder delay/paired fields, retiring packets
        // FFmpeg accepts but drops without ever returning a frame.
        constexpr static size_t MaxPendingSubmissions{64};
        std::map<u64, Submission> submissions;
        bool initialized{}; //!< If the underlying FFmpeg decoder could be created
        bool hiddenFrame{}; //!< If the current frame is decode-only and produces no visible output (VP9 show_existing_frame handling)

        Codec(const DeviceState &state, const Registers &registers);

        /**
         * @brief Composes a standards-compliant bitstream packet for the current decode operation from guest memory
         */
        virtual span<const u8> ComposeBitstream() = 0;

        /**
         * @return The IOVA of the luma plane of the surface this operation decodes into, used as the frame's queue key
         */
        virtual u64 GetOutputLumaAddress() = 0;

        virtual OutputSurface GetOutputSurface() = 0;

      public:
        virtual ~Codec() = default;

        /**
         * @brief Runs one decode operation as configured in the register file, pushing the decoded frame into the supplied queue
         */
        void Decode(FrameQueue &frameQueue, u64 streamId);
    };
}
