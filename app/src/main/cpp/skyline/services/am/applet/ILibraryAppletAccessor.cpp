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
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const bool completed{IsAppletCompleted()};
        response.Push<u8>(completed);
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
        auto storage{request.PopService<IStorage>(0, session)};
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd && swkbdInteractiveTraceCount < 12) {
            const auto bytes{storage->GetSpan()};
            u32 command{};
            if (bytes.size() >= sizeof(command))
                std::memcpy(&command, bytes.data(), sizeof(command));
            LOGI("SWKBDTrace PushInteractiveInData: command=0x{:X}, size=0x{:X}", command, bytes.size());
            ++swkbdInteractiveTraceCount;
        }
        applet->PushInteractiveDataToApplet(std::move(storage));
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
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd && !swkbdInteractiveEventTraced) {
            swkbdInteractiveEventTraced = true;
            LOGI("SWKBDTrace GetPopInteractiveOutDataEvent: handle=0x{:X}", popInteractiveOutDataEventHandle);
        }
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto requestedAppletResourceUserId{request.Pop<u64>()};
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd && !swkbdIndirectHandleTraced) {
            swkbdIndirectHandleTraced = true;
            LOGI("SWKBDTrace GetIndirectLayerConsumerHandle: mode={}, exited={}, pid=0x{:X}, aruid=0x{:X}, expectedAruid=0x{:X}",
                 static_cast<u32>(appletMode), exited, request.pid, requestedAppletResourceUserId, appletResourceUserId);
        }
        if (exited || appletMode != applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay || !request.pid ||
            requestedAppletResourceUserId != appletResourceUserId)
            return result::ObjectInvalid;
        if (!indirectLayerHandle ||
            !indirectLayers->Get(indirectLayerHandle, request.pid, requestedAppletResourceUserId)) {
            indirectLayers->Unregister(indirectLayerHandle);
            indirectLayerHandle = indirectLayers->Register(applet, request.pid, requestedAppletResourceUserId);
        }
        response.Push<u64>(indirectLayerHandle);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(unknown170Event));
        return {};
    }
}
