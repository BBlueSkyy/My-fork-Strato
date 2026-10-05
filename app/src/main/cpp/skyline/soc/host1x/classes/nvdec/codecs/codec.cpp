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
        if (!decoder.SendPacket(packet, token, hiddenFrame))
            return;
        submissions.emplace(token, Submission{surfaceKey, output, hiddenFrame});
        while (submissions.size() > MaxPendingSubmissions)
            submissions.erase(submissions.begin());

        const auto materialize{[&](const Submission &metadata, const AVFrame *frame) -> bool {
            try {
                WriteDecodedSurface(state, metadata.output, frame);
                return true;
            } catch (const std::exception &e) {
                // Retain the software VIC path for invalid guest destinations.
                LOGW("NVDEC output surface write failed: {}", e.what());
                return false;
            }
        }};

        // Drain presentation output completely. For H.264 this also completes the
        // current decode operation even when its picture remains in the DPB for
        // presentation reordering.
        std::vector<AVFramePtr> presentationFrames;
        while (auto frame{decoder.ReceiveFrame()})
            presentationFrames.push_back(std::move(frame));

        // NVDEC picture/reference surfaces become visible in decode order, not in
        // display order. get_buffer2 retains the current H.264 decode target, whose
        // storage is complete once receive_frame() has been drained to EAGAIN.
        for (auto &frame : decoder.TakeDecodedSurfaces()) {
            auto submission{submissions.find(static_cast<u64>(frame->pts))};
            if (submission == submissions.end())
                continue;

            submission->second.materialized |= materialize(submission->second, frame.get());
        }

        // Presentation remains in libavcodec output order. Keep its metadata until
        // the delayed frame arrives so VIC can still select the originating surface.
        for (auto &frame : presentationFrames) {
            auto submission{submissions.find(static_cast<u64>(frame->pts))};
            if (submission == submissions.end()) {
                LOGW("NVDEC frame has no matching decode submission");
                continue;
            }

            auto metadata{submission->second};
            if (!metadata.materialized)
                materialize(metadata, frame.get());
            submissions.erase(submission);

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

        // Decode-only pictures have no future presentation event to retire their
        // bookkeeping. Once their guest surface is complete they are finished.
        for (auto submission{submissions.begin()}; submission != submissions.end();) {
            if (submission->second.hidden && submission->second.materialized)
                submission = submissions.erase(submission);
            else
                ++submission;
        }
    }
}
