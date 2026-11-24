// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include <applet/applet_creator.h>
#include <cstring>
#include <utility>
#include <algorithm>
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
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            swkbdIpcTrace = std::make_unique<skyline::applet::swkbd::SoftwareKeyboardTraceGate>();
        stateChangeEventHandle = state.process->InsertItem(stateChangeEvent);
        popNormalOutDataEventHandle = state.process->InsertItem(popNormalOutDataEvent);
        popInteractiveOutDataEventHandle = state.process->InsertItem(popInteractiveOutDataEvent);
    }

    void ILibraryAppletAccessor::TraceIpc(u32 command, const char *stage, u32 phase, u64 detail, u64 extra) {
        using Gate = skyline::applet::swkbd::SoftwareKeyboardTraceGate;
        if (!swkbdIpcTrace || !AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Info))
            return;
        const auto epoch{std::min(swkbdIpcPushes.load(std::memory_order_relaxed), 8U)};
        const auto decision{swkbdIpcTrace->Take(Gate::Point::Accessor, {command, phase, detail, epoch})};
        if (decision == Gate::Decision::Limit)
            LOGI("[SWKBD] AM stateEvent=0x{:X} diagnostic detail limit reached", stateChangeEventHandle);
        if (decision == Gate::Decision::Emit)
            LOGI("[SWKBD] AM stateEvent=0x{:X} ipc={} stage={} epoch={} detail=0x{:X} extra=0x{:X}",
                 stateChangeEventHandle, command, stage, epoch, detail, extra);
    }

    ILibraryAppletAccessor::~ILibraryAppletAccessor() {
        indirectLayers->Unregister(indirectLayerHandle);
    }
    Result ILibraryAppletAccessor::StartApplet() {
        TraceIpc(10, "start-enter", 0);
        stateChangeEvent->ResetSignal();
        const auto result{applet->Start()};
        TraceIpc(10, "handler-return", 1, result.raw);
        return result;
    }

    bool ILibraryAppletAccessor::IsAppletCompleted() const {
        std::scoped_lock lock{kernel::type::KSyncObject::syncObjectMutex};
        return stateChangeEvent->signalled;
    }

    Result ILibraryAppletAccessor::GetAppletStateChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(0, "enter", 0);
        response.copyHandles.push_back(stateChangeEventHandle);
        TraceIpc(0, "handler-return", 1);
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(1, "enter-before-sync-lock", 0);
        const bool completed{IsAppletCompleted()};
        response.Push<u8>(completed);
        TraceIpc(1, "handler-return", 1, completed);
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
        TraceIpc(30, "enter", 0);
        const auto result{applet->GetResult()};
        TraceIpc(30, "handler-return", 1, result.raw);
        return result;
    }

    Result ILibraryAppletAccessor::PresetLibraryAppletGpuTimeSliceZero(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        TraceIpc(60, "enter", 0);
        TraceIpc(60, "handler-return", 1);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown90(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        TraceIpc(90, "enter", 0);
        TraceIpc(90, "handler-return", 1);
        return {};
    }

    Result ILibraryAppletAccessor::PushInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        applet->PushNormalDataToApplet(request.PopService<IStorage>(0, session));
        return {};
    }

    Result ILibraryAppletAccessor::PushInteractiveInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        // Trace the service boundary before resolving storage, not only inside the applet.
        // Only the first eight pushes get admission markers; polling is deduplicated below.
        const u32 call{swkbdIpcTrace ? swkbdIpcPushes.fetch_add(1, std::memory_order_relaxed) + 1 : 0};
        const bool traceAdmission{call && call <= 8};
        if (traceAdmission) TraceIpc(103, "enter-before-storage", 0, call);
        auto data{request.PopService<IStorage>(0, session)};
        if (traceAdmission) {
            TraceIpc(103, "storage-resolved", 2, call);
            const auto bytes{data->GetSpan()};
            u32 command{0xFFFFFFFFU};
            if (bytes.size() >= sizeof(command)) std::memcpy(&command, bytes.data(), sizeof(command));
            TraceIpc(103, "packet-before-applet", 3, command, bytes.size());
        }
        applet->PushInteractiveDataToApplet(std::move(data));
        if (traceAdmission) TraceIpc(103, "handler-return", 1, call);
        return {};
    }

    Result ILibraryAppletAccessor::PopOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(101, "enter-before-output-lock", 0);
        if (auto outStorage{applet->PopNormalAndClear()}) {
            manager.RegisterService(outStorage, session, response);
            TraceIpc(101, "handler-return", 1, 0);
            return {};
        }
        TraceIpc(101, "handler-return", 1, result::NotAvailable.raw);
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::PopInteractiveOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(104, "enter-before-output-lock", 0);
        if (auto outStorage{applet->PopInteractiveAndClear()}) {
            manager.RegisterService(outStorage, session, response);
            TraceIpc(104, "handler-return", 1, 0);
            return {};
        }
        TraceIpc(104, "handler-return", 1, result::NotAvailable.raw);
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(105, "enter", 0);
        response.copyHandles.push_back(popNormalOutDataEventHandle);
        TraceIpc(105, "handler-return", 1);
        return {};
    }

    Result ILibraryAppletAccessor::GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(106, "enter", 0);
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        TraceIpc(106, "handler-return", 1);
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(120, "enter", 0);
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        TraceIpc(120, "handler-return", 1);
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        TraceIpc(160, "enter-before-indirect-registry", 0);
        const auto requestedAppletResourceUserId{request.Pop<u64>()};
        if (exited || appletMode != applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay || !request.pid ||
            requestedAppletResourceUserId != appletResourceUserId) {
            TraceIpc(160, "handler-return", 1, result::ObjectInvalid.raw);
            return result::ObjectInvalid;
        }
        if (!indirectLayerHandle ||
            !indirectLayers->Get(indirectLayerHandle, request.pid, requestedAppletResourceUserId)) {
            indirectLayers->Unregister(indirectLayerHandle);
            indirectLayerHandle = indirectLayers->Register(applet, request.pid, requestedAppletResourceUserId);
        }
        response.Push<u64>(indirectLayerHandle);
        TraceIpc(160, "handler-return", 1, 0, indirectLayerHandle);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        TraceIpc(170, "enter", 0);
        response.copyHandles.push_back(state.process->InsertItem(unknown170Event));
        TraceIpc(170, "handler-return", 1);
        return {};
    }
}
