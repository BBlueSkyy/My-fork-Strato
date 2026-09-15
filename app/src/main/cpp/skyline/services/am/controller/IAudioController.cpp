// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include "IAudioController.h"

namespace skyline::service::am {
    namespace {
        constexpr float MinVolume{0.0F};
        constexpr float MaxVolume{1.0F};
    }

    IAudioController::IAudioController(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IAudioController::SetExpectedMasterVolume(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        mainAppletVolume = std::clamp(request.Pop<float>(), MinVolume, MaxVolume);
        libraryAppletVolume = std::clamp(request.Pop<float>(), MinVolume, MaxVolume);
        return {};
    }

    Result IAudioController::GetMainAppletExpectedMasterVolume(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<float>(mainAppletVolume);
        return {};
    }

    Result IAudioController::GetLibraryAppletExpectedMasterVolume(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<float>(libraryAppletVolume);
        return {};
    }

    Result IAudioController::ChangeMainAppletMasterVolume(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        mainAppletVolume = std::clamp(request.Pop<float>(), MinVolume, MaxVolume);
        fadeTimeNs = request.Pop<i64>();
        return {};
    }

    Result IAudioController::SetTransparentVolumeRate(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        transparentVolumeRate = std::clamp(request.Pop<float>(), MinVolume, MaxVolume);
        return {};
    }
}
