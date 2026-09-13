// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include <applet/applet_creator.h>
#include "ILibraryAppletAccessor.h"

namespace skyline::service::am {
    ILibraryAppletAccessor::ILibraryAppletAccessor(const DeviceState &state, ServiceManager &manager,
                                                   skyline::applet::AppletId appletId,
                                                   applet::LibraryAppletMode appletMode)
        : BaseService(state, manager),
          stateChangeEvent(std::make_shared<type::KEvent>(state, false)),
          popNormalOutDataEvent(std::make_shared<type::KEvent>(state, false)),
          popInteractiveOutDataEvent(std::make_shared<type::KEvent>(state, false)),
          unknown170Event(std::make_shared<type::KEvent>(state, false)),
          appletId(appletId), appletMode(appletMode),
          applet(skyline::applet::CreateApplet(state, manager, appletId, stateChangeEvent,
                                               popNormalOutDataEvent, popInteractiveOutDataEvent,
                                               appletMode)), indirectLayers(manager.indirectLayers) {
        if (appletMode == applet::LibraryAppletMode::PartialForegroundWithIndirectDisplay)
            indirectLayerHandle = indirectLayers->Register(applet);
        stateChangeEventHandle = state.process->InsertItem(stateChangeEvent);
        popNormalOutDataEventHandle = state.process->InsertItem(popNormalOutDataEvent);
        popInteractiveOutDataEventHandle = state.process->InsertItem(popInteractiveOutDataEvent);
        LOGI("SWKBD-TRACE AM accessor created: id=0x{:X}, mode=0x{:X}, indirectHandle=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode), indirectLayerHandle);
    }

    ILibraryAppletAccessor::~ILibraryAppletAccessor() {
        LOGI("SWKBD-TRACE AM accessor destroy: id=0x{:X}, indirectHandle=0x{:X}",
             static_cast<u32>(appletId), indirectLayerHandle);
        indirectLayers->Unregister(indirectLayerHandle);
    }

    Result ILibraryAppletAccessor::StartApplet() {
        LOGI("SWKBD-TRACE AM Start enter: id=0x{:X}, mode=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode));
        stateChangeEvent->ResetSignal();
        const auto result{applet->Start()};
        LOGI("SWKBD-TRACE AM Start returned: id=0x{:X}, mode=0x{:X}, result=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode), static_cast<u32>(result));
        return result;
    }

    bool ILibraryAppletAccessor::IsAppletCompleted() const {
        return stateChangeEvent->signalled;
    }

    Result ILibraryAppletAccessor::GetAppletStateChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        LOGI("SWKBD-TRACE AM GetAppletStateChangedEvent: id=0x{:X}, handle=0x{:X}, signalled={}",
             static_cast<u32>(appletId), stateChangeEventHandle, stateChangeEvent->signalled);
        response.copyHandles.push_back(stateChangeEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        const bool completed{IsAppletCompleted()};
        LOGI("SWKBD-TRACE AM IsCompleted: id=0x{:X}, completed={}", static_cast<u32>(appletId), completed);
        response.Push<u8>(completed);
        return {};
    }

    Result ILibraryAppletAccessor::Start(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return StartApplet();
    }

    Result ILibraryAppletAccessor::RequestExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGI("SWKBD-TRACE AM RequestExit: id=0x{:X}", static_cast<u32>(appletId));
        // Strato frontends are in-process rather than independent HOS processes. Marking the
        // state event completes the same observable AM contract without killing the application.
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::Terminate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGI("SWKBD-TRACE AM Terminate: id=0x{:X}", static_cast<u32>(appletId));
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::GetResult(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        const auto result{applet->GetResult()};
        LOGI("SWKBD-TRACE AM GetResult: id=0x{:X}, result=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(result));
        return result;
    }

    Result ILibraryAppletAccessor::PresetLibraryAppletGpuTimeSliceZero(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::Unknown90(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::PushInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto storage{request.PopService<IStorage>(0, session)};
        LOGI("SWKBD-TRACE AM PushInData enter: id=0x{:X}, mode=0x{:X}, size=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode),
             storage ? storage->GetSpan().size() : 0);
        applet->PushNormalDataToApplet(std::move(storage));
        LOGI("SWKBD-TRACE AM PushInData returned: id=0x{:X}", static_cast<u32>(appletId));
        return {};
    }

    Result ILibraryAppletAccessor::PushInteractiveInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto storage{request.PopService<IStorage>(0, session)};
        size_t storageSize{};
        u32 command{0xFFFFFFFFu};
        if (storage) {
            const auto data{storage->GetSpan()};
            storageSize = data.size();
            if (data.size() >= sizeof(command))
                std::memcpy(&command, data.data(), sizeof(command));
            else if (!data.empty())
                command = data.front();
        }
        LOGI("SWKBD-TRACE AM PushInteractiveInData enter: id=0x{:X}, mode=0x{:X}, size=0x{:X}, command=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode), storageSize, command);
        applet->PushInteractiveDataToApplet(std::move(storage));
        LOGI("SWKBD-TRACE AM PushInteractiveInData returned: id=0x{:X}, command=0x{:X}",
             static_cast<u32>(appletId), command);
        return {};
    }

    Result ILibraryAppletAccessor::PopOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (auto outStorage{applet->PopNormalAndClear()}) {
            LOGI("SWKBD-TRACE AM PopOutData: id=0x{:X}, size=0x{:X}",
                 static_cast<u32>(appletId), outStorage->GetSpan().size());
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        LOGI("SWKBD-TRACE AM PopOutData: id=0x{:X}, no data", static_cast<u32>(appletId));
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::PopInteractiveOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (auto outStorage{applet->PopInteractiveAndClear()}) {
            LOGI("SWKBD-TRACE AM PopInteractiveOutData: id=0x{:X}, size=0x{:X}",
                 static_cast<u32>(appletId), outStorage->GetSpan().size());
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        LOGI("SWKBD-TRACE AM PopInteractiveOutData: id=0x{:X}, no data", static_cast<u32>(appletId));
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        LOGI("SWKBD-TRACE AM GetPopOutDataEvent: id=0x{:X}, handle=0x{:X}, signalled={}",
             static_cast<u32>(appletId), popNormalOutDataEventHandle, popNormalOutDataEvent->signalled);
        response.copyHandles.push_back(popNormalOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        LOGI("SWKBD-TRACE AM GetPopInteractiveOutDataEvent: id=0x{:X}, handle=0x{:X}, signalled={}",
             static_cast<u32>(appletId), popInteractiveOutDataEventHandle, popInteractiveOutDataEvent->signalled);
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (!indirectLayerHandle || !indirectLayers->Get(indirectLayerHandle)) {
            LOGI("SWKBD-TRACE AM GetIndirectLayerConsumerHandle invalid: id=0x{:X}, handle=0x{:X}",
                 static_cast<u32>(appletId), indirectLayerHandle);
            return result::ObjectInvalid;
        }
        LOGI("SWKBD-TRACE AM GetIndirectLayerConsumerHandle: id=0x{:X}, handle=0x{:X}",
             static_cast<u32>(appletId), indirectLayerHandle);
        response.Push<u64>(indirectLayerHandle);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(unknown170Event));
        return {};
    }
}
