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

    NvDecClass::StreamState &NvDecClass::GetStream(u64 streamId) {
        auto [it, inserted]{streams.try_emplace(streamId)};
        if (inserted) {
            it->second = std::make_unique<StreamState>();
            frameQueue.OpenStream(streamId);
            LOGI("XV2-TRACE NVDEC stream created stream={}", streamId);
        }
        return *it->second;
    }

    void NvDecClass::CallMethod(u32 method, u32 argument, u64 streamId) {
        constexpr u32 CodecIdMethodId{offsetof(nvdec::Registers, codecId) / sizeof(u64)};
        constexpr u32 ExecuteMethodId{offsetof(nvdec::Registers, execute) / sizeof(u64)};

        if (method >= nvdec::RegisterCount) {
            LOGW("NVDEC method out of range: 0x{:X} argument: 0x{:X}", method, argument);
            return;
        }

        auto &stream{GetStream(streamId)};
        stream.registers.raw[method] = argument;

        if (method == CodecIdMethodId)
            CreateCodec(streamId, stream);
        else if (method == ExecuteMethodId)
            Execute(streamId, stream);
    }

    void NvDecClass::CreateCodec(u64 streamId, StreamState &stream) {
        // Repeated codec-ID writes within one nvhost stream must preserve reference frame state,
        // while a different stream owns an independent decoder and register file.
        if (stream.codec && stream.activeCodecId == stream.registers.codecId)
            return;

        stream.codec.reset();
        stream.activeCodecId = stream.registers.codecId;

        switch (stream.registers.codecId) {
            case nvdec::CodecId::H264:
                stream.codec = std::make_unique<nvdec::H264>(state, stream.registers);
                break;
            case nvdec::CodecId::Vp8:
                stream.codec = std::make_unique<nvdec::Vp8>(state, stream.registers);
                break;
            case nvdec::CodecId::Vp9:
                stream.codec = std::make_unique<nvdec::Vp9>(state, stream.registers);
                break;
            default:
                LOGW("Unimplemented NVDEC codec: {}", static_cast<u64>(stream.registers.codecId));
                break;
        }

        if (stream.codec)
            LOGI("XV2-TRACE NVDEC codec created codec={} stream={}",
                 static_cast<u64>(stream.registers.codecId), streamId);
    }

    void NvDecClass::Execute(u64 streamId, StreamState &stream) {
        if (!stream.codec) {
            LOGW("NVDEC execute without a codec for stream: {}", streamId);
            return;
        }

        try {
            stream.codec->Decode(frameQueue, streamId);
        } catch (const std::exception &e) {
            LOGE("NVDEC execute failed for stream {}: {}", streamId, e.what());
        }
    }

    void NvDecClass::CloseStream(u64 streamId) {
        streams.erase(streamId);
        frameQueue.CloseStream(streamId);
        LOGI("XV2-TRACE NVDEC stream destroyed stream={}", streamId);
    }
}
