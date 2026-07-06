// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <soc/host1x/frame_queue.h>
#include "nvdec/codecs/h264.h"
#include "nvdec/codecs/vp8.h"
#include "nvdec/codecs/vp9.h"
#include "nvdec.h"

namespace skyline::soc::host1x {
    NvDecClass::NvDecClass(const DeviceState &state, FrameQueue &frameQueue, std::function<void()> opDoneCallback)
        : state(state),
          frameQueue(frameQueue),
          opDoneCallback(std::move(opDoneCallback)) {}

    NvDecClass::~NvDecClass() = default;

    void NvDecClass::CallMethod(u32 method, u32 argument) {
        constexpr u32 CodecIdMethodId{offsetof(nvdec::Registers, codecId) / sizeof(u64)};
        constexpr u32 ExecuteMethodId{offsetof(nvdec::Registers, execute) / sizeof(u64)};

        if (method >= nvdec::RegisterCount) {
            LOGW("NVDEC method out of range: 0x{:X} argument: 0x{:X}", method, argument);
            return;
        }

        registers.raw[method] = argument;

        if (method == CodecIdMethodId)
            CreateCodec();
        else if (method == ExecuteMethodId)
            Execute();
    }

    void NvDecClass::CreateCodec() {
        // The guest writes the codec ID with every submission, recreating the codec would discard
        // the decoder's reference frame state and break every inter-predicted frame
        if (codec && activeCodecId == registers.codecId)
            return;

        codec.reset();
        activeCodecId = registers.codecId;

        switch (registers.codecId) {
            case nvdec::CodecId::H264:
                codec = std::make_unique<nvdec::H264>(state, registers);
                break;
            case nvdec::CodecId::Vp8:
                codec = std::make_unique<nvdec::Vp8>(state, registers);
                break;
            case nvdec::CodecId::Vp9:
                codec = std::make_unique<nvdec::Vp9>(state, registers);
                break;
            default:
                LOGW("Unimplemented NVDEC codec: {}", static_cast<u64>(registers.codecId));
                break;
        }

        if (codec)
            LOGD("Created NVDEC codec: {}", static_cast<u64>(registers.codecId));
    }

    void NvDecClass::Execute() {
        if (!codec) {
            LOGW("NVDEC execute without a codec, the frame will be missing");
            return;
        }

        try {
            codec->Decode(frameQueue);
        } catch (const std::exception &e) {
            LOGE("NVDEC execute failed: {}", e.what());
        }
    }
}
