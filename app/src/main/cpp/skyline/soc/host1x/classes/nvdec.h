// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

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
        const DeviceState &state;
        FrameQueue &frameQueue; //!< Queue for handing off decoded frames to VIC
        std::function<void()> opDoneCallback;
        nvdec::Registers registers{};
        std::unique_ptr<nvdec::Codec> codec; //!< The active codec instance, created on a codec ID register write, null when the codec is unsupported
        nvdec::CodecId activeCodecId{}; //!< The codec ID the current codec instance was created for

        /**
         * @brief Instantiates the codec implementation matching the current codec ID register
         */
        void CreateCodec();

        /**
         * @brief Runs one decode operation on the active codec
         * @note Any errors are logged rather than thrown so a malformed operation can never take down the FIFO thread
         */
        void Execute();

      public:
        NvDecClass(const DeviceState &state, FrameQueue &frameQueue, std::function<void()> opDoneCallback);

        ~NvDecClass();

        void CallMethod(u32 method, u32 argument);
    };
}
