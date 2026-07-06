// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

extern "C" {
#include <libavutil/frame.h>
}

#include "codec.h"

namespace skyline::soc::host1x::nvdec {
    Codec::Codec(const DeviceState &state, const Registers &registers) : state(state), registers(registers) {}

    void Codec::Decode(FrameQueue &frameQueue) {
        if (!initialized) {
            LOGW("Decode without an initialised decoder");
            return;
        }

        auto packet{ComposeBitstream()};
        if (packet.empty())
            return;

        u64 surfaceKey{GetOutputLumaAddress()};
        if (!decoder.SendPacket(packet, surfaceKey))
            return;

        // Drain every frame the decoder has ready, a reordering decoder may hold frames across
        // operations so each frame is pushed under the surface key its own submission carried
        while (auto frame{decoder.ReceiveFrame()})
            frameQueue.PushFrame(static_cast<u64>(frame->pts), std::move(frame));
    }
}
