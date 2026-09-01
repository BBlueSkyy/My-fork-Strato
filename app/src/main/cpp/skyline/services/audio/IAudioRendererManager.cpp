// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 yuzu Emulator Project (https://github.com/yuzu-emu/)

#include <audio_core/common/audio_renderer_parameter.h>
#include <audio_core/common/feature_support.h>
#include <audio_core/audio_render_manager.h>
#include <common/utils.h>
#include <audio.h>
#include "IAudioRenderer.h"
#include "IAudioDevice.h"
#include "IAudioRendererManager.h"

namespace skyline::service::audio {
    namespace {
        void LogRendererParameters(const char *operation, const AudioCore::AudioRendererParameterInternal &params) {
            LOGI("{} parameters: revision=0x{:08X} (REV{}), sampleRate={}, sampleCount={}, mixes={}, "
                 "subMixes={}, voices={}, sinks={}, effects={}, perfFrames={}, voiceDropEnabled={}, "
                 "renderingDevice={}, executionMode={}, splitterInfos={}, splitterDestinations={}, "
                 "externalContextSize=0x{:X}",
                 operation, params.revision, AudioCore::GetRevisionNum(params.revision),
                 params.sample_rate, params.sample_count, params.mixes, params.sub_mixes,
                 params.voices, params.sinks, params.effects, params.perf_frames,
                 static_cast<u32>(params.voice_drop_enabled),
                 static_cast<u32>(params.rendering_device),
                 static_cast<u32>(params.execution_mode), params.splitter_infos,
                 params.splitter_destinations, params.external_context_size);
        }
    }

    IAudioRendererManager::IAudioRendererManager(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager) {}

    Result IAudioRendererManager::OpenAudioRenderer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        LOGI("OpenAudioRenderer: entered");
        const auto &params{request.Pop<AudioCore::AudioRendererParameterInternal>()};
        const u32 cmifPadding{request.Pop<u32>()}; // CMIF padding after the 0x34-byte REV12+ parameter.
        u64 transferMemorySize{request.Pop<u64>()};
        u64 appletResourceUserId{request.Pop<u64>()};
        auto transferMemoryHandle{request.copyHandles.at(0)};
        auto processHandle{request.copyHandles.at(1)};

        LogRendererParameters("OpenAudioRenderer", params);
        LOGI("OpenAudioRenderer request: cmifPadding=0x{:08X}, transferMemorySize=0x{:X}, "
             "appletResourceUserId=0x{:X}, transferMemoryHandle=0x{:X}, processHandle=0x{:X}, "
             "copyHandleCount={}",
             cmifPadding, transferMemorySize, appletResourceUserId, transferMemoryHandle,
             processHandle, request.copyHandles.size());

        i32 sessionId{state.audio->audioRendererManager->GetSessionId()};
        if (sessionId == -1) {
            LOGW("Out of audio renderer sessions!");
            return Result{Service::Audio::ResultOutOfSessions};
        }

        auto renderer{std::make_shared<IAudioRenderer>(
            state, manager, *state.audio->audioRendererManager, params, transferMemorySize,
            processHandle, appletResourceUserId, sessionId)};

        const auto initializationResult{renderer->GetInitializationResult()};
        if (initializationResult.IsError()) {
            LOGI("OpenAudioRenderer initialization failed: raw=0x{:X}, module={}, id={}",
                 initializationResult.raw, static_cast<u32>(initializationResult.module),
                 static_cast<u32>(initializationResult.id));
            state.audio->audioRendererManager->ReleaseSessionId(sessionId);
            return Result{initializationResult};
        }

        manager.RegisterService(renderer, session, response);

        LOGI("OpenAudioRenderer initialization succeeded: sessionId={}", sessionId);

        return {};
    }

    Result IAudioRendererManager::GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        LOGI("GetWorkBufferSize: entered");
        const auto &params{request.Pop<AudioCore::AudioRendererParameterInternal>()};
        LogRendererParameters("GetWorkBufferSize", params);

        u64 size{};
        auto err{state.audio->audioRendererManager->GetWorkBufferSize(params, size)};
        LOGI("GetWorkBufferSize result: size=0x{:X}, raw=0x{:X}, module={}, id={}",
             size, err.raw, static_cast<u32>(err.module), static_cast<u32>(err.id));
        if (err.IsError())
            LOGW("Failed to calculate work buffer size");

        response.Push<u64>(size);

        return Result{err};
    }

    Result IAudioRendererManager::GetAudioDeviceService(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u64 appletResourceUserId{request.Pop<u64>()};
        manager.RegisterService(std::make_shared<IAudioDevice>(state, manager, appletResourceUserId, util::MakeMagic<u32>("REV1")), session, response);
        return {};
    }

    Result IAudioRendererManager::GetAudioDeviceServiceWithRevisionInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        u32 revision{request.Pop<u32>()};
        u64 appletResourceUserId{request.Pop<u64>()};
        manager.RegisterService(std::make_shared<IAudioDevice>(state, manager, appletResourceUserId, revision), session, response);
        return {};
    }

}
