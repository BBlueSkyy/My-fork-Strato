// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <unordered_map>
#include <common.h>
#include "nvdec/registers.h"

namespace skyline::soc::host1x {
    class FrameQueue;

    namespace nvdec {
        class Codec;
    }

    /**
     * @brief The NVDEC Host1x class implements hardware accelerated video decoding for the VP9/VP8/H264/VC1 codecs
     * @url https://github.com/NVIDIA/open-gpu-doc (clc5b0 video decoder class)
     */
    class NvDecClass {
      private:
        struct StreamState {
            nvdec::Registers registers{};
            std::unique_ptr<nvdec::Codec> codec;
            nvdec::CodecId activeCodecId{};
        };

        const DeviceState &state;
        FrameQueue &frameQueue;
        std::function<void()> opDoneCallback;
        std::unordered_map<u64, std::unique_ptr<StreamState>> streams;

        StreamState &GetStream(u64 streamId);
        void CreateCodec(u64 streamId, StreamState &stream);
        void Execute(u64 streamId, StreamState &stream);

      public:
        NvDecClass(const DeviceState &state, FrameQueue &frameQueue, std::function<void()> opDoneCallback);

        ~NvDecClass();

        void CallMethod(u32 method, u32 argument, u64 streamId);

        void CloseStream(u64 streamId);
    };
}
