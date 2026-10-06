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
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] accessor created mode=0x{:X} aruid=0x{:X} stateEvent=0x{:X} normalOutEvent=0x{:X} interactiveOutEvent=0x{:X}",
                 static_cast<u32>(appletMode), appletResourceUserId, stateChangeEventHandle,
                 popNormalOutDataEventHandle, popInteractiveOutDataEventHandle);
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
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] GetAppletStateChangedEvent handle=0x{:X}", stateChangeEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const bool completed{IsAppletCompleted()};
        response.Push<u8>(completed);
        return {};
    }

    Result ILibraryAppletAccessor::Start(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] Start applet mode=0x{:X}", static_cast<u32>(appletMode));
        const auto result{StartApplet()};
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] Start return result=0x{:X}", result.raw);
        return result;
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
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] PresetLibraryAppletGpuTimeSliceZero");
        return {};
    }

    Result ILibraryAppletAccessor::Unknown90(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::PushInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] PushInData begin");
        applet->PushNormalDataToApplet(request.PopService<IStorage>(0, session));
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] PushInData end");
        return {};
    }

    Result ILibraryAppletAccessor::PushInteractiveInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] PushInteractiveInData begin");
        applet->PushInteractiveDataToApplet(request.PopService<IStorage>(0, session));
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] PushInteractiveInData end");
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
            return {};
        }
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popNormalOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] GetPopInteractiveOutDataEvent handle=0x{:X}", popInteractiveOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto requestedAppletResourceUserId{request.Pop<u64>()};
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] GetIndirectLayerConsumerHandle request mode=0x{:X} pid=0x{:X} requestedAruid=0x{:X} expectedAruid=0x{:X} exited={}",
                 static_cast<u32>(appletMode), request.pid, requestedAppletResourceUserId, appletResourceUserId, exited);
        if (exited || appletMode != applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay || !request.pid ||
            requestedAppletResourceUserId != appletResourceUserId) {
            if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
                LOGI("[SWKBD-IPC] GetIndirectLayerConsumerHandle reject ObjectInvalid");
            return result::ObjectInvalid;
        }
        if (!indirectLayerHandle ||
            !indirectLayers->Get(indirectLayerHandle, request.pid, requestedAppletResourceUserId)) {
            indirectLayers->Unregister(indirectLayerHandle);
            indirectLayerHandle = indirectLayers->Register(applet, request.pid, requestedAppletResourceUserId);
        }
        response.Push<u64>(indirectLayerHandle);
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] GetIndirectLayerConsumerHandle return handle=0x{:X}", indirectLayerHandle);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const KHandle handle{state.process->InsertItem(unknown170Event)};
        response.copyHandles.push_back(handle);
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-IPC] Unknown170 handle=0x{:X}", handle);
        return {};
    }
}
