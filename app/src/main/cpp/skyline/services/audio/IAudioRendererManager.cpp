// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2022 yuzu Emulator Project (https://github.com/yuzu-emu/)

#include <audio_core/common/audio_renderer_parameter.h>
#include <audio_core/audio_render_manager.h>
#include <audio_core/common/feature_support.h>
#include <audio_core/renderer/command/effect/biquad_filter_and_mix.h>
#include <audio_core/renderer/command/mix/mix_ramp.h>
#include <algorithm>
#include <common/utils.h>
#include <audio.h>
#include "IAudioRenderer.h"
#include "IAudioDevice.h"
#include "IAudioRendererManager.h"

namespace skyline::service::audio {
    namespace {
        // Experimental guest ABI estimate: retain the previous splitter-command
        // reservation and keep the extra native fused-command storage private.
        // The older estimate is not yet validated against Horizon REV12-15.
        constexpr u64 GuestWorkBufferSize(u32 revision, s32 destinations, u64 hostSize) {
            using namespace AudioCore::AudioRenderer;
            if (revision < 12 || revision > 15 || destinations <= 0)
                return hostSize;
            constexpr u64 legacySize{sizeof(MixRampCommand)};
            constexpr u64 fusedSize{std::max({sizeof(MixRampCommand),
                sizeof(BiquadFilterAndMixCommand), sizeof(MultiTapBiquadFilterAndMixCommand)})};
            // Round the removed reservation DOWN to a page, so the guest size
            // never falls below the page-aligned estimate using legacy commands.
            const u64 excess{static_cast<u64>(destinations) * AudioCore::MaxMixBuffers * (fusedSize - legacySize)};
            const u64 removable{excess & ~u64{0xFFF}};
            return removable < hostSize ? hostSize - removable : hostSize;
        }

        static_assert(GuestWorkBufferSize(11, 384, 0x38B000) == 0x38B000);
        static_assert(GuestWorkBufferSize(15, 0, 0x38B000) == 0x38B000);
        static_assert(GuestWorkBufferSize(15, -1, 0x38B000) == 0x38B000);
        static_assert(GuestWorkBufferSize(15, 384, 0x38B000) <= 0x38B000);
        static_assert((GuestWorkBufferSize(15, 384, 0x38B000) & 0xFFF) == 0);

        u64 GuestWorkBufferSize(const AudioCore::AudioRendererParameterInternal &params, u64 hostSize) {
            return GuestWorkBufferSize(AudioCore::GetRevisionNum(params.revision),
                                       params.splitter_destinations, hostSize);
        }
    }

    IAudioRendererManager::IAudioRendererManager(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager) {}

    Result IAudioRendererManager::OpenAudioRenderer(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto &params{request.Pop<AudioCore::AudioRendererParameterInternal>()};
        request.Pop<u32>(); // CMIF padding after the 0x34-byte REV12+ parameter.
        u64 transferMemorySize{request.Pop<u64>()};
        u64 appletResourceUserId{request.Pop<u64>()};
        auto transferMemoryHandle{request.copyHandles.at(0)};
        auto processHandle{request.copyHandles.at(1)};

        LOGD("[AUDREN-OPEN] revision=0x{:08X} transfer_size=0x{:X} sample_rate={} sample_count={} mixes={} sub_mixes={} voices={} sinks={} effects={} perf_frames={} splitter_infos={} splitter_destinations={} external_context_size=0x{:X}",
             params.revision, transferMemorySize, params.sample_rate, params.sample_count,
             params.mixes, params.sub_mixes, params.voices, params.sinks, params.effects,
             params.perf_frames, params.splitter_infos, params.splitter_destinations,
             params.external_context_size);

        u64 hostRequired{};
        const auto sizeResult{state.audio->audioRendererManager->GetWorkBufferSize(params, hostRequired)};
        if (sizeResult.IsError())
            return Result{sizeResult};

        const u64 guestRequired{GuestWorkBufferSize(params, hostRequired)};
        if (transferMemorySize < guestRequired)
            return Result{Service::Audio::ResultInsufficientBuffer};

        // IAudioRenderer's KTransferMemory shim owns a private host vector.
        // It does not map the guest transfer memory. Both the shim and the
        // Audio Core allocator must still receive the complete native size.
        const u64 hostSize{std::max(hostRequired, transferMemorySize)};
        LOGD("[AUDREN-SPLIT] guest_required=0x{:X} guest_provided=0x{:X} host_required=0x{:X} host_allocated=0x{:X}",
             guestRequired, transferMemorySize, hostRequired, hostSize);

        i32 sessionId{state.audio->audioRendererManager->GetSessionId()};
        if (sessionId == -1) {
            LOGW("Out of audio renderer sessions!");
            return Result{Service::Audio::ResultOutOfSessions};
        }

        auto renderer{std::make_shared<IAudioRenderer>(
            state, manager, *state.audio->audioRendererManager, params, hostSize,
            processHandle, appletResourceUserId, sessionId)};

        const auto initializationResult{renderer->GetInitializationResult()};
        if (initializationResult.IsError()) {
            state.audio->audioRendererManager->ReleaseSessionId(sessionId);
            return Result{initializationResult};
        }

        manager.RegisterService(renderer, session, response);

        return {};
    }

    Result IAudioRendererManager::GetWorkBufferSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto &params{request.Pop<AudioCore::AudioRendererParameterInternal>()};

        u64 size{};
        auto err{state.audio->audioRendererManager->GetWorkBufferSize(params, size)};
        if (err.IsError())
            LOGW("Failed to calculate work buffer size");

        LOGD("[AUDREN-SIZE] revision=0x{:08X} sample_rate={} sample_count={} mixes={} sub_mixes={} voices={} sinks={} effects={} perf_frames={} voice_drop={} unk21={} rendering_device={} execution_mode={} splitter_infos={} splitter_destinations={} external_context_size=0x{:X} size=0x{:X} error={}",
             params.revision, params.sample_rate, params.sample_count, params.mixes,
             params.sub_mixes, params.voices, params.sinks, params.effects, params.perf_frames,
             static_cast<u32>(params.voice_drop_enabled), static_cast<u32>(params.unk_21),
             static_cast<u32>(params.rendering_device), static_cast<u32>(params.execution_mode),
             params.splitter_infos, params.splitter_destinations, params.external_context_size,
             size, err.IsError());

        const u64 guestSize{err.IsError() ? size : GuestWorkBufferSize(params, size)};
        LOGD("[AUDREN-SPLIT] query guest_required=0x{:X} host_required=0x{:X}", guestSize, size);
        response.Push<u64>(guestSize);

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
