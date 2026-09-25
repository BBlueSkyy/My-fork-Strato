// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include <applet/applet_creator.h>
#include <cstring>
#include <utility>
#include "ILibraryAppletAccessor.h"

namespace skyline::service::am {
    ILibraryAppletAccessor::ILibraryAppletAccessor(const DeviceState &state, ServiceManager &manager,
                                                   skyline::applet::AppletId appletId,
                                                   applet::LibraryAppletMode appletMode,
                                                   u64 appletResourceUserId)
        : BaseService(state, manager),
          stateChangeEvent(std::make_shared<type::KEvent>(state, false)),
          popNormalOutDataEvent(std::make_shared<type::KEvent>(state, false)),
          popInteractiveOutDataEvent(std::make_shared<type::KEvent>(state, false)),
          unknown170Event(std::make_shared<type::KEvent>(state, false)),
          appletId(appletId), appletMode(appletMode),
          applet(skyline::applet::CreateApplet(state, manager, appletId, stateChangeEvent,
                                               popNormalOutDataEvent, popInteractiveOutDataEvent,
                                               appletMode)), indirectLayers(manager.indirectLayers),
          appletResourceUserId(appletResourceUserId) {
        stateChangeEventHandle = state.process->InsertItem(stateChangeEvent);
        popNormalOutDataEventHandle = state.process->InsertItem(popNormalOutDataEvent);
        popInteractiveOutDataEventHandle = state.process->InsertItem(popInteractiveOutDataEvent);
        LOGD("Applet accessor for {} ID created with appletMode 0x{:X}", ToString(appletId), appletMode);
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd &&
            appletMode == applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay) {
            LOGI("SWKBD accessor events: stateChanged handle=0x{:X} object={}, "
                 "normalOut handle=0x{:X} object={}, "
                 "interactiveOut handle=0x{:X} object={}",
                 stateChangeEventHandle, fmt::ptr(stateChangeEvent.get()),
                 popNormalOutDataEventHandle, fmt::ptr(popNormalOutDataEvent.get()),
                 popInteractiveOutDataEventHandle, fmt::ptr(popInteractiveOutDataEvent.get()));
        }
    }

    ILibraryAppletAccessor::~ILibraryAppletAccessor() {
        indirectLayers->Unregister(indirectLayerHandle);
    }
    Result ILibraryAppletAccessor::StartApplet() {
        stateChangeEvent->ResetSignal();
        return applet->Start();
    }

    bool ILibraryAppletAccessor::IsAppletCompleted() const {
        std::scoped_lock lock{kernel::type::KSyncObject::syncObjectMutex};
        return stateChangeEvent->signalled;
    }

    Result ILibraryAppletAccessor::GetAppletStateChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(stateChangeEventHandle);
        LOGI("GetAppletStateChangedEvent: handle=0x{:X}, object={}", stateChangeEventHandle, fmt::ptr(stateChangeEvent.get()));
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const bool completed{IsAppletCompleted()};
        response.Push<u8>(completed);
        LOGI("IsCompleted: completed={}", completed);
        return {};
    }

    Result ILibraryAppletAccessor::Start(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return StartApplet();
    }

    Result ILibraryAppletAccessor::RequestExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        applet->RequestExit();
        indirectLayers->Unregister(indirectLayerHandle);
        indirectLayerHandle = 0;
        exited = true;
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::Terminate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        applet->RequestExit();
        indirectLayers->Unregister(indirectLayerHandle);
        indirectLayerHandle = 0;
        exited = true;
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::GetResult(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return applet->GetResult();
    }

    Result ILibraryAppletAccessor::PresetLibraryAppletGpuTimeSliceZero(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::Unknown90(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::PushInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        applet->PushNormalDataToApplet(request.PopService<IStorage>(0, session));
        return {};
    }

    Result ILibraryAppletAccessor::PushInteractiveInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto data{request.PopService<IStorage>(0, session)};
        const auto dataSpan{data->GetSpan()};
        u32 command{};
        const bool hasCommand{dataSpan.size() >= sizeof(command)};
        if (hasCommand) {
            std::memcpy(&command, dataSpan.data(), sizeof(command));
            LOGI("PushInteractiveInData: command=0x{:X}, size=0x{:X}", command, dataSpan.size());
        } else {
            LOGI("PushInteractiveInData: command=<truncated>, size=0x{:X}", dataSpan.size());
        }

        applet->PushInteractiveDataToApplet(std::move(data));

        if (hasCommand && command == 0xE &&
            appletId == skyline::applet::AppletId::LibraryAppletSwkbd &&
            appletMode == applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay) {
            bool stateChangedSignalled{};
            bool normalOutSignalled{};
            bool interactiveOutSignalled{};
            {
                std::scoped_lock lock{kernel::type::KSyncObject::syncObjectMutex};
                stateChangedSignalled = stateChangeEvent->signalled;
                normalOutSignalled = popNormalOutDataEvent->signalled;
                interactiveOutSignalled = popInteractiveOutDataEvent->signalled;
            }

            LOGI("PRECALC ACCESSOR: applet=0x{:X}, mode=0x{:X}, exited={}, completed={}, "
                 "stateChangedHandle=0x{:X}, stateChangedSignalled={}, "
                 "normalOutHandle=0x{:X}, normalOutSignalled={}, "
                 "interactiveOutHandle=0x{:X}, interactiveOutSignalled={}, "
                 "pid=0x{:X}, aruid=0x{:X}, consumerHandle=0x{:X}",
                 static_cast<u32>(appletId), static_cast<u32>(appletMode), exited, stateChangedSignalled,
                 stateChangeEventHandle, stateChangedSignalled,
                 popNormalOutDataEventHandle, normalOutSignalled,
                 popInteractiveOutDataEventHandle, interactiveOutSignalled,
                 request.pid, appletResourceUserId, indirectLayerHandle);
        }
        return {};
    }

    Result ILibraryAppletAccessor::PopOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (auto outStorage{applet->PopNormalAndClear()}) {
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::PopInteractiveOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (auto outStorage{applet->PopInteractiveAndClear()}) {
            manager.RegisterService(outStorage, session, response);
            LOGI("PopInteractiveOutData: Success");
            return {};
        }
        LOGI("PopInteractiveOutData: NotAvailable");
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popNormalOutDataEventHandle);
        LOGI("GetPopOutDataEvent: handle=0x{:X}, object={}", popNormalOutDataEventHandle,
             fmt::ptr(popNormalOutDataEvent.get()));
        return {};
    }

    Result ILibraryAppletAccessor::GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        LOGI("GetPopInteractiveOutDataEvent: handle=0x{:X}, object={}", popInteractiveOutDataEventHandle,
             fmt::ptr(popInteractiveOutDataEvent.get()));
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto requestedAppletResourceUserId{request.Pop<u64>()};
        if (exited || appletMode != applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay || !request.pid ||
            requestedAppletResourceUserId != appletResourceUserId)
            return result::ObjectInvalid;
        if (!indirectLayerHandle ||
            !indirectLayers->Get(indirectLayerHandle, request.pid, requestedAppletResourceUserId)) {
            indirectLayers->Unregister(indirectLayerHandle);
            indirectLayerHandle = indirectLayers->Register(applet, request.pid, requestedAppletResourceUserId);
        }
        response.Push<u64>(indirectLayerHandle);
        LOGI("GetIndirectLayerConsumerHandle: success, handle=0x{:X}, pid=0x{:X}, ARUID=0x{:X}",
             indirectLayerHandle, request.pid, requestedAppletResourceUserId);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(unknown170Event));
        return {};
    }
}
