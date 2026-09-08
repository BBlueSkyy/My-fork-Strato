// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cstring>
#include <common/settings.h>
#include <kernel/types/KProcess.h>
#include <loader/loader.h>
#include <services/am/storage/IStorage.h>
#include "IAppletCommonFunctions.h"

namespace skyline::service::am {
    namespace {
        constexpr Result ResultNotAvailable{128, 2};
    }

    IAppletCommonFunctions::IAppletCommonFunctions(const DeviceState &state, ServiceManager &manager, std::shared_ptr<AppletState> appletState)
        : BaseService(state, manager), appletState(std::move(appletState)) {}

    Result IAppletCommonFunctions::SetTerminateResult(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto terminateResult{request.Pop<Result>()};
        {
            std::scoped_lock lock{appletState->mutex};
            appletState->terminateResult = terminateResult;
        }
        LOGI("Applet set termination result: {}", terminateResult.raw);
        return {};
    }

    Result IAppletCommonFunctions::ReadThemeStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const u64 offset{request.Pop<u64>()};
        auto &out{request.outputBuf.at(0)};

        if (offset >= appletState->themeStorage.size()) {
            response.Push<u64>(0);
            return {};
        }

        const size_t transferSize{std::min(out.size(), appletState->themeStorage.size() - static_cast<size_t>(offset))};
        {
            std::scoped_lock lock{appletState->mutex};
            std::memcpy(out.data(), appletState->themeStorage.data() + offset, transferSize);
        }
        response.Push<u64>(transferSize);
        return {};
    }

    Result IAppletCommonFunctions::WriteThemeStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const u64 offset{request.Pop<u64>()};
        const auto &in{request.inputBuf.at(0)};

        if (offset >= appletState->themeStorage.size())
            return {};

        const size_t transferSize{std::min(in.size(), appletState->themeStorage.size() - static_cast<size_t>(offset))};
        std::scoped_lock lock{appletState->mutex};
        std::memcpy(appletState->themeStorage.data() + offset, in.data(), transferSize);
        return {};
    }

    Result IAppletCommonFunctions::PushToAppletBoundChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto storage{request.PopService<IStorage>(0, session)};
        std::scoped_lock lock{appletState->mutex};
        appletState->appletBoundChannel.emplace_back(std::move(storage));
        return {};
    }

    Result IAppletCommonFunctions::TryPopFromAppletBoundChannel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::shared_ptr<IStorage> storage;
        {
            std::scoped_lock lock{appletState->mutex};
            if (appletState->appletBoundChannel.empty())
                return ResultNotAvailable;
            storage = std::move(appletState->appletBoundChannel.front());
            appletState->appletBoundChannel.pop_front();
        }
        manager.RegisterService(storage, session, response);
        return {};
    }

    Result IAppletCommonFunctions::GetDisplayLogicalResolution(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (*state.settings->isDocked) {
            response.Push<i32>(1920);
            response.Push<i32>(1080);
        } else {
            response.Push<i32>(1280);
            response.Push<i32>(720);
        }
        return {};
    }

    Result IAppletCommonFunctions::SetDisplayMagnification(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const float x{request.Pop<float>()};
        const float y{request.Pop<float>()};
        const float width{request.Pop<float>()};
        const float height{request.Pop<float>()};

        if (x < 0.0F || y < 0.0F || width < 0.0F || height < 0.0F || x > 1.0F || y > 1.0F || width > 1.0F || height > 1.0F)
            return Result{128, 506};

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

    Result IAppletCommonFunctions::IsHomeButtonShortPressedBlocked(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        response.Push<u8>(appletState->homeButtonBlocked || appletState->homeButtonShortAndLongBlocked);
        return {};
    }

    Result IAppletCommonFunctions::IsVrModeCurtainRequired(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result IAppletCommonFunctions::IsSleepRequiredByHighTemperature(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result IAppletCommonFunctions::IsSleepRequiredByLowBattery(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result IAppletCommonFunctions::SetCpuBoostRequestPriority(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->cpuBoostRequestPriority = request.Pop<i32>();
        return {};
    }

    Result IAppletCommonFunctions::SetHandlingCaptureButtonShortPressedMessageEnabledForApplet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->handlingCaptureButtonShortPressedEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result IAppletCommonFunctions::SetHandlingCaptureButtonLongPressedMessageEnabledForApplet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->handlingCaptureButtonLongPressedEnabled = request.Pop<u8>() != 0;
        return {};
    }

    Result IAppletCommonFunctions::SetBlockingCaptureButtonInEntireSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->blockingCaptureButtonInEntireSystem = request.Pop<u8>() != 0;
        return {};
    }

    Result IAppletCommonFunctions::SetApplicationCoreUsageMode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->applicationCoreUsageMode = request.Pop<u32>();
        return {};
    }

    Result IAppletCommonFunctions::GetCurrentApplicationId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(state.loader->nacp->nacpContents.saveDataOwnerId);
        return {};
    }

    Result IAppletCommonFunctions::IsSystemAppletHomeMenu(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(false);
        return {};
    }

    Result IAppletCommonFunctions::SetGpuTimeSliceBoost(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->gpuTimeSliceBoost = request.Pop<u64>();
        return {};
    }

    Result IAppletCommonFunctions::SetGpuTimeSliceBoostDueToApplication(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        std::scoped_lock lock{appletState->mutex};
        appletState->gpuTimeSliceBoostDueToApplication = request.Pop<u64>();
        return {};
    }

    Result IAppletCommonFunctions::GetGpuErrorEventForApplet(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{state.process->InsertItem(appletState->gpuErrorEvent)};
        response.copyHandles.push_back(handle);
        return {};
    }
}
