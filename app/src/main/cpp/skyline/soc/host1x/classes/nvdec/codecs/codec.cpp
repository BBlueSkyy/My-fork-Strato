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
        LOGD("NVDEC submit, surface: 0x{:X}, hidden: {}, packet size: 0x{:X}",
             surfaceKey, hiddenFrame, packet.size());
        if (!decoder.SendPacket(packet, surfaceKey, hiddenFrame))
            return;

        // Drain every frame the decoder has ready. Visible frames carry the target surface IOVA
        // in PTS across decoder reordering; decode-only frames deliberately have no PTS and must
        // never be exposed to VIC as presentation frames.
        while (auto frame{decoder.ReceiveFrame()}) {
            if (frame->pts == AV_NOPTS_VALUE) {
                LOGD("NVDEC decoded a hidden frame, not queueing it for VIC");
                continue;
            }

            LOGD("NVDEC decoded presentation frame, submitted surface: 0x{:X}, format: {}, dimensions: {}x{}, linesizes: [{}, {}, {}]",
                 static_cast<u64>(frame->pts), frame->format, frame->width, frame->height,
                 frame->linesize[0], frame->linesize[1], frame->linesize[2]);
            frameQueue.PushPresentationFrame(streamId, static_cast<u64>(frame->pts), std::move(frame));
        }
    }
}
