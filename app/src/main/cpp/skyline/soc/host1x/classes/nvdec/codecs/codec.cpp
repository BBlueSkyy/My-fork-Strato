// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include "codec.h"

namespace skyline::soc::host1x::nvdec {
    Codec::Codec(const DeviceState &state, const Registers &registers) : state(state), registers(registers) {}

    void Codec::Decode(FrameQueue &frameQueue) {
        if (!initialized) {
            LOGW("Decode without an initialised decoder");
            return;
        }

        auto packet{ComposeBitstream()};
        if (packet.empty() || !decoder.SendPacket(packet))
            return;

        if (hiddenFrame)
            return;

        auto frame{decoder.ReceiveFrame()};
        if (!frame) {
            LOGW("Failed to decode a frame for luma surface: 0x{:X}", GetOutputLumaAddress());
            return;
        }

        frameQueue.PushFrame(GetOutputLumaAddress(), std::move(frame));
    }
}
