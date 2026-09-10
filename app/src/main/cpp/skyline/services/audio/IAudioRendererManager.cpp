// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 yuzu Emulator Project (https://github.com/yuzu-emu/)

#include <audio_core/common/audio_renderer_parameter.h>
#include <audio_core/audio_render_manager.h>
#include <common/utils.h>
#include <audio.h>
#include <nce/diagnostics.h>
#include "IAudioRenderer.h"
#include "IAudioDevice.h"
#include "IAudioRendererManager.h"

namespace skyline::service::audio {
    namespace {
        void LogParameters(const char *operation, const AudioCore::AudioRendererParameterInternal &p) {
            LOGI("{} revision=0x{:08X}, sampleRate={}, sampleCount={}, mixes={}, subMixes={}, voices={}, sinks={}, effects={}, perfFrames={}, voiceDrop={}, renderingDevice={}, executionMode={}, splitterInfos={}, splitterDestinations={}, externalContextSize=0x{:X}",
                 operation, p.revision, p.sample_rate, p.sample_count, p.mixes, p.sub_mixes,
                 p.voices, p.sinks, p.effects, p.perf_frames, static_cast<u32>(p.voice_drop_enabled),
                 static_cast<u32>(p.rendering_device), static_cast<u32>(p.execution_mode),
                 p.splitter_infos, p.splitter_destinations, p.external_context_size);
        }
    }

    IAudioRendererManager::IAudioRendererManager(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager) {}

    Result IAudioRendererManager::OpenAudioRenderer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        LOGI("OpenAudioRenderer entered");
        nce::diagnostics::DisarmAudioTrace();
        const auto &params{request.Pop<AudioCore::AudioRendererParameterInternal>()};
        LogParameters("OpenAudioRenderer", params);
        request.Pop<u32>(); // CMIF padding after the 0x34-byte REV12+ parameter.
        u64 transferMemorySize{request.Pop<u64>()};
        u64 appletResourceUserId{request.Pop<u64>()};
        auto transferMemoryHandle{request.copyHandles.at(0)};
        auto processHandle{request.copyHandles.at(1)};
        LOGI("OpenAudioRenderer transferSize=0x{:X}, transferHandle=0x{:X}, processHandle=0x{:X}", transferMemorySize, transferMemoryHandle, processHandle);

        i32 sessionId{state.audio->audioRendererManager->GetSessionId()};
        if (sessionId == -1) {
            LOGW("Out of audio renderer sessions!");
            return Result{Service::Audio::ResultOutOfSessions};
        }

        auto renderer{std::make_shared<IAudioRenderer>(
            state, manager, *state.audio->audioRendererManager, params, transferMemorySize,
            processHandle, appletResourceUserId, sessionId)};

        const auto initializationResult{renderer->GetInitializationResult()};
        LOGI("OpenAudioRenderer initialization result=0x{:X}", initializationResult.raw);
        if (initializationResult.IsError()) {
            state.audio->audioRendererManager->ReleaseSessionId(sessionId);
            return Result{initializationResult};
        }

        manager.RegisterService(renderer, session, response);

        return {};
    }

    Result IAudioRendererManager::GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        nce::diagnostics::ArmAudioTrace();
        const auto &params{request.Pop<AudioCore::AudioRendererParameterInternal>()};
        LogParameters("GetWorkBufferSize", params);

        u64 size{};
        auto err{state.audio->audioRendererManager->GetWorkBufferSize(params, size)};
        LOGI("GetWorkBufferSize result=0x{:X}, size=0x{:X}", err.raw, size);
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
