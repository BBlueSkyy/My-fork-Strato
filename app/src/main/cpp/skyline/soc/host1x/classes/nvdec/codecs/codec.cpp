// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

extern "C" {
#include <libavutil/frame.h>
}

#include "codec.h"

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
        LOGI("[VideoDiag] nvdec-packet stream={} surface=0x{:X} hidden={} size=0x{:X}",
             streamId, surfaceKey, hiddenFrame, packet.size());
        if (!decoder.SendPacket(packet, surfaceKey, hiddenFrame)) {
            LOGI("[VideoDiag] nvdec-send-failed stream={} surface=0x{:X}", streamId, surfaceKey);
            return;
        }

        size_t decodedFrames{};

        // Drain every frame the decoder has ready. Visible frames carry the target surface IOVA
        // in PTS across decoder reordering; decode-only frames deliberately have no PTS and must
        // never be exposed to VIC as presentation frames.
        while (auto frame{decoder.ReceiveFrame()}) {
            decodedFrames++;

            if (frame->pts == AV_NOPTS_VALUE) {
                LOGI("[VideoDiag] nvdec-frame-hidden stream={} index={}", streamId, decodedFrames);
                continue;
            }

            LOGI("[VideoDiag] nvdec-frame stream={} index={} surface=0x{:X} format={} size={}x{} lines=[{},{},{}]",
                 streamId, decodedFrames, static_cast<u64>(frame->pts), frame->format, frame->width, frame->height,
                 frame->linesize[0], frame->linesize[1], frame->linesize[2]);
            frameQueue.PushPresentationFrame(streamId, static_cast<u64>(frame->pts), std::move(frame));
        }

        LOGI("[VideoDiag] nvdec-drain-complete stream={} decoded={}", streamId, decodedFrames);
    }
}
