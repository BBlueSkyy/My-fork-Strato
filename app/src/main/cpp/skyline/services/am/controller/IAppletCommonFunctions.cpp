// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <loader/loader.h>
#include "IAppletCommonFunctions.h"

namespace skyline::service::am {
    IAppletCommonFunctions::IAppletCommonFunctions(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result IAppletCommonFunctions::SetDisplayMagnification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const float x{request.Pop<float>()};
        const float y{request.Pop<float>()};
        const float width{request.Pop<float>()};
        const float height{request.Pop<float>()};

        std::scoped_lock lock{appletState->mutex};
        appletState->displayMagnificationX = x;
        appletState->displayMagnificationY = y;
        appletState->displayMagnificationWidth = width;
        appletState->displayMagnificationHeight = height;
        return {};
    }

    Result IAppletCommonFunctions::SetHomeButtonDoubleClickEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->homeButtonDoubleClickEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result IAppletCommonFunctions::GetHomeButtonDoubleClickEnabled(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->homeButtonDoubleClickEnabled);
        return {};
    }

    Result IAppletCommonFunctions::SetCpuBoostRequestPriority(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->cpuBoostRequestPriority = request.Pop<i32>();
        return {};
    }

    Result IAppletCommonFunctions::GetCurrentApplicationId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(state.loader->nacp->nacpContents.saveDataOwnerId & ~0xFULL);
        return {};
    }

    Result IAppletCommonFunctions::SetGpuTimeSliceBoost(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const i64 timeSpan{request.Pop<i64>()};
        std::scoped_lock lock{appletState->mutex};
        appletState->gpuTimeSliceBoost = static_cast<u64>(timeSpan);
        return {};
    }

    Result IAppletCommonFunctions::Unknown350(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u16>(0);
        return {};
    }
}
