// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <opus_multistream.h>
#include <services/serviceman.h>
#include <kernel/types/KTransferMemory.h>

#include "IHardwareOpusDecoderManager.h"
#include "IHardwareOpusDecoder.h"

namespace skyline::service::codec {
    static u32 CalculateBufferSize(i32 sampleRate, i32 channelCount, i32 useLargerFrameSize = 0) {
        u32 requiredSize{static_cast<u32>(opus_decoder_get_size(channelCount))};
        requiredSize += MaxInputBufferSize + CalculateOutBufferSize(sampleRate, channelCount, useLargerFrameSize ? MaxFrameSizeEx : MaxFrameSizeNormal);
        return requiredSize;
    }

    static u32 CalculateMultiStreamBufferSize(i32 sampleRate, i32 channelCount, i32 totalStreamCount, i32 stereoStreamCount, i32 useLargerFrameSize = 0) {
        u32 requiredSize{static_cast<u32>(opus_multistream_decoder_get_size(totalStreamCount, stereoStreamCount))};
        requiredSize += MaxInputBufferSize + CalculateOutBufferSize(sampleRate, channelCount, useLargerFrameSize ? MaxFrameSizeEx : MaxFrameSizeNormal);
        return requiredSize;
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoder(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        LOGD("Creating Opus decoder: Sample rate: {}, Channel count: {}, Work buffer handle: 0x{:X} (Size: 0x{:X})", sampleRate, channelCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, sampleRate, channelCount, workBufferSize, workBuffer), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};

        response.Push<u32>(CalculateBufferSize(sampleRate, channelCount));
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        request.Pop<i32>(); // Just padding
        u32 workBufferSize{request.Pop<u32>()};
        KHandle workBuffer{request.copyHandles.at(0)};

        LOGD("Creating Opus decoder: Sample rate: {}, Channel count: {}, Work buffer handle: 0x{:X} (Size: 0x{:X})", sampleRate, channelCount, workBuffer, workBufferSize);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, sampleRate, channelCount, workBufferSize, workBuffer, useLargerFrameSize), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        request.Pop<i32>(); // Just padding

        response.Push<u32>(CalculateBufferSize(sampleRate, channelCount, useLargerFrameSize));
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto params{request.inputBuf.at(0).as<MultiStreamParameters>()};
        KHandle workBufferHandle{request.copyHandles.at(0)};
        u32 workBufferSize{static_cast<u32>(state.process->GetHandle<kernel::type::KTransferMemory>(workBufferHandle)->host.size())};

        LOGD("Creating multi-stream Opus decoder: Sample rate: {}, Channel count: {}, Streams: {}, Stereo streams: {}",
             params.sampleRate, params.channelCount, params.streamCount, params.stereoStreamCount);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, params.sampleRate, params.channelCount, workBufferSize, workBufferHandle), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStream(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto params{request.inputBuf.at(0).as<MultiStreamParameters>()};

        response.Push<u32>(CalculateMultiStreamBufferSize(params.sampleRate, params.channelCount, params.streamCount, params.stereoStreamCount));
        return {};
    }

    Result IHardwareOpusDecoderManager::OpenHardwareOpusDecoderForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto params{request.inputBuf.at(0).as<MultiStreamParameters>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        KHandle workBufferHandle{request.copyHandles.at(0)};
        u32 workBufferSize{static_cast<u32>(state.process->GetHandle<kernel::type::KTransferMemory>(workBufferHandle)->host.size())};

        LOGD("Creating multi-stream Opus decoder (Ex): Sample rate: {}, Channel count: {}, Streams: {}, Stereo streams: {}",
             params.sampleRate, params.channelCount, params.streamCount, params.stereoStreamCount);

        manager.RegisterService(std::make_shared<IHardwareOpusDecoder>(state, manager, params.sampleRate, params.channelCount, workBufferSize, workBufferHandle, useLargerFrameSize), session, response);
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto params{request.inputBuf.at(0).as<MultiStreamParameters>()};
        i32 useLargerFrameSize{request.Pop<i32>()};

        response.Push<u32>(CalculateMultiStreamBufferSize(params.sampleRate, params.channelCount, params.streamCount, params.stereoStreamCount, useLargerFrameSize));
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeExEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // [16.0.0+] Same parameters as GetWorkBufferSizeEx
        i32 sampleRate{request.Pop<i32>()};
        i32 channelCount{request.Pop<i32>()};
        i32 useLargerFrameSize{request.Pop<i32>()};
        request.Pop<i32>(); // Just padding

        response.Push<u32>(CalculateBufferSize(sampleRate, channelCount, useLargerFrameSize));
        return {};
    }

    Result IHardwareOpusDecoderManager::GetWorkBufferSizeForMultiStreamExEx(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // [16.0.0+] Same parameters as GetWorkBufferSizeForMultiStreamEx
        auto params{request.inputBuf.at(0).as<MultiStreamParameters>()};
        i32 useLargerFrameSize{request.Pop<i32>()};

        response.Push<u32>(CalculateMultiStreamBufferSize(params.sampleRate, params.channelCount, params.streamCount, params.stereoStreamCount, useLargerFrameSize));
        return {};
    }

    Result IHardwareOpusDecoderManager::Cmd100(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }
}
