// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

extern "C" {
#include <libavutil/frame.h>
}

#include "codec.h"
#include <soc/host1x/classes/nvdec/surface_writer.h>

namespace skyline::soc::host1x::nvdec {
    Codec::Codec(const DeviceState &state, const Registers &registers) : state(state), registers(registers) {}

    void Codec::Decode(FrameQueue &frameQueue, u64 streamId) {
        if (!initialized) {
            LOGW("Decode without an initialised decoder");
            return;
        }

        auto packet{ComposeBitstream()};
        if (packet.empty())
            return;

        u64 surfaceKey{GetOutputLumaAddress()};
        LOGD("NVDEC submit, surface: 0x{:X}, hidden: {}, packet size: 0x{:X}",
             surfaceKey, hiddenFrame, packet.size());
        u64 token{nextSubmission++};
        auto output{GetOutputSurface()};
        if (!decoder.SendPacket(packet, token))
            return;
        submissions.emplace(token, Submission{surfaceKey, output, hiddenFrame});
        while (submissions.size() > MaxPendingSubmissions)
            submissions.erase(submissions.begin());

        // A unique token carries the complete originating destination through FFmpeg
        // reordering. An IOVA alone cannot identify submissions that reuse a surface.
        while (auto frame{decoder.ReceiveFrame()}) {
            auto submission{submissions.find(static_cast<u64>(frame->pts))};
            if (submission == submissions.end()) {
                LOGW("NVDEC frame has no matching decode submission");
                continue;
            }
            auto metadata{submission->second};
            submissions.erase(submission);
            try {
                WriteDecodedSurface(state, metadata.output, frame.get());
            } catch (const std::exception &e) {
                // Preserve the software VIC path even if a guest supplies an invalid
                // NVDEC destination. Never synthesize a surface or a completion marker.
                LOGW("NVDEC output surface write failed: {}", e.what());
            }
            if (metadata.hidden)
                continue;
            if (av_frame_apply_cropping(frame.get(), 0) < 0) {
                LOGW("NVDEC decoded frame has invalid crop margins");
                continue;
            }
            frame->pts = static_cast<i64>(metadata.surfaceKey);

            LOGD("NVDEC decoded presentation frame, submitted surface: 0x{:X}, format: {}, dimensions: {}x{}, linesizes: [{}, {}, {}]",
                 static_cast<u64>(frame->pts), frame->format, frame->width, frame->height,
                 frame->linesize[0], frame->linesize[1], frame->linesize[2]);
            frameQueue.PushPresentationFrame(streamId, metadata.surfaceKey, std::move(frame));
        }
    }
}
