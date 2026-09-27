// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 yuzu Emulator Project (https://github.com/yuzu-emu/)

#include <audio.h>
#include <kernel/types/KProcess.h>
#include "IAudioOut.h"

namespace skyline::service::audio {
    namespace {
        // The first few sessions show one full open/start/close cycle without
        // flooding logcat when a title repeatedly recreates AudioOut.
        std::atomic<u32> audioOutSessionsLogged{};
    }

    IAudioOut::IAudioOut(const DeviceState &state, ServiceManager &manager, size_t sessionId,
                         std::string_view deviceName, AudioCore::AudioOut::AudioOutParameter parameters,
                         KHandle handle, u32 appletResourceUserId)
        :  BaseService{state, manager},
           releaseEvent{std::make_shared<type::KEvent>(state, false)},
           releaseEventWrapper{[releaseEvent = this->releaseEvent]() { releaseEvent->Signal(); },
                               [releaseEvent = this->releaseEvent]() { releaseEvent->ResetSignal(); }},
           audioOutSessionId{sessionId},
           diagnosticSession{audioOutSessionsLogged.fetch_add(1, std::memory_order_relaxed) < 3},
           impl{std::make_shared<AudioCore::AudioOut::Out>(state.audio->audioSystem, *state.audio->audioOutManager, &releaseEventWrapper, sessionId)} {

        const auto result{impl->GetSystem().Initialize(std::string{deviceName}, parameters, handle, appletResourceUserId)};
        if (diagnosticSession)
            LOGI("AudioOut open: session={} name='{}' rate={} channels={} result=0x{:X}",
                 sessionId, deviceName, static_cast<s32>(parameters.sample_rate),
                 static_cast<u16>(parameters.channel_count), u32{result});
        if (result.IsError())
            LOGW("Failed to initialise Audio Out: 0x{:X}", u32{result});
    }

    IAudioOut::~IAudioOut() {
        if (diagnosticSession)
            LOGI("AudioOut close: session={} state={} played={}", audioOutSessionId,
                 static_cast<u32>(impl->GetState()), impl->GetPlayedSampleCount());
        impl->Free();
    }

    Result IAudioOut::GetAudioOutState(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto audioState{static_cast<u32>(impl->GetState())};
        if (diagnosticSession && loggedStates.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGI("AudioOut state: session={} state={}", audioOutSessionId, audioState);
        response.Push(audioState);
        return {};
    }

    Result IAudioOut::StartAudioOut(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto result{impl->StartSystem()};
        if (diagnosticSession && loggedStarts.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGI("AudioOut start: session={} result=0x{:X} state={}", audioOutSessionId,
                 u32{result}, static_cast<u32>(impl->GetState()));
        return Result{result};
    }

    Result IAudioOut::StopAudioOut(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto result{impl->StopSystem()};
        if (diagnosticSession && loggedStops.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGI("AudioOut stop: session={} result=0x{:X}", audioOutSessionId, u32{result});
        return Result{result};
    }

    Result IAudioOut::AppendAudioOutBuffer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto &buffer{request.inputBuf.at(0).as<AudioCore::AudioOut::AudioOutBuffer>()};
        auto tag{request.Pop<u64>()};

        const auto result{impl->AppendBuffer(buffer, tag)};
        if (diagnosticSession && loggedAppends.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGI("AudioOut append: session={} tag=0x{:X} samples=0x{:X} size=0x{:X} capacity=0x{:X} offset=0x{:X} result=0x{:X}",
                 audioOutSessionId, tag, static_cast<u64>(buffer.samples), static_cast<u64>(buffer.size), static_cast<u64>(buffer.capacity),
                 static_cast<u64>(buffer.offset), u32{result});
        return Result{result};
    }

    Result IAudioOut::RegisterBufferEvent(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(releaseEvent)};
        LOGD("Buffer Release Event Handle: 0x{:X}", handle);
        response.copyHandles.push_back(handle);
        return {};
    }

    Result IAudioOut::GetReleasedAudioOutBuffer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto maxCount{request.outputBuf.at(0).size() >> 3};

        std::vector<u64> releasedBuffers(maxCount);
        auto count{impl->GetReleasedBuffers(releasedBuffers)};
        if (diagnosticSession && loggedReleases.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGI("AudioOut release: session={} count={} capacity={} played={}",
                 audioOutSessionId, count, maxCount, impl->GetPlayedSampleCount());

        request.outputBuf.at(0).copy_from(releasedBuffers);
        response.Push<u32>(count);
        return {};
    }

    Result IAudioOut::ContainsAudioOutBuffer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto tag{request.Pop<u64>()};
        response.Push(static_cast<u32>(impl->ContainsAudioBuffer(tag)));
        return {};
    }

    Result IAudioOut::GetAudioOutBufferCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(impl->GetBufferCount());
        return {};
    }

    Result IAudioOut::GetAudioOutPlayedSampleCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(impl->GetPlayedSampleCount());
        return {};
    }

    Result IAudioOut::FlushAudioOutBuffers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(static_cast<u32>(impl->FlushAudioOutBuffers()));
        return {};
    }

    Result IAudioOut::SetAudioOutVolume(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto volume{request.Pop<float>()};
        impl->SetVolume(volume);
        if (diagnosticSession && loggedVolumes.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGI("AudioOut volume: session={} volume={}", audioOutSessionId, volume);
        return {};
    }

    Result IAudioOut::GetAudioOutVolume(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(impl->GetVolume());
        return {};
    }
}
