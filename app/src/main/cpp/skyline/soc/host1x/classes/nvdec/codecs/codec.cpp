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
        if (!decoder.SendPacket(packet, token, hiddenFrame)) {
            LOGI("[GrandiaNvdecDiag] decode-submit token={} surface=0x{:X} accepted=false pending={}",
                 token, surfaceKey, submissions.size());
            return;
        }
        submissions.emplace(token, Submission{surfaceKey, output, hiddenFrame});
        LOGI("[GrandiaNvdecDiag] decode-submit token={} surface=0x{:X} accepted=true hidden={} packet=0x{:X} pending={}",
             token, surfaceKey, hiddenFrame, packet.size(), submissions.size());
        while (submissions.size() > MaxPendingSubmissions)
            submissions.erase(submissions.begin());

        const auto materialize{[&](const Submission &metadata, const AVFrame *frame) {
            try {
                WriteDecodedSurface(state, metadata.output, frame);
            } catch (const std::exception &e) {
                // Retain the software VIC path for invalid guest destinations.
                LOGW("NVDEC output surface write failed: {}", e.what());
            }
        }};

        // A unique token carries the complete originating destination through FFmpeg
        // reordering. An IOVA alone cannot identify submissions that reuse a surface.
        size_t producedFrames{};
        while (auto frame{decoder.ReceiveFrame()}) {
            producedFrames++;
            auto submission{submissions.find(static_cast<u64>(frame->pts))};
            if (submission == submissions.end()) {
                LOGW("NVDEC frame has no matching decode submission");
                continue;
            }
            auto metadata{submission->second};
            const u64 outputToken{static_cast<u64>(frame->pts)};
            submissions.erase(submission);
            LOGI("[GrandiaNvdecDiag] decode-output token={} surface=0x{:X} hidden={} size={}x{} remainingPending={}",
                 outputToken, metadata.surfaceKey, metadata.hidden, frame->width, frame->height, submissions.size());
            materialize(metadata, frame.get());
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

        auto decodedReferences{decoder.TakeDecodedReferences()};
        const size_t referenceFrames{decodedReferences.size()};
        for (auto &frame : decodedReferences) {
            auto submission{submissions.find(static_cast<u64>(frame->pts))};
            // A returned decode-only AVFrame was already written above.
            if (submission == submissions.end())
                continue;
            materialize(submission->second, frame.get());
            submissions.erase(submission);
        }

        LOGI("[GrandiaNvdecDiag] decode-drain submittedToken={} produced={} references={} pending={}",
             token, producedFrames, referenceFrames, submissions.size());
    }
}
