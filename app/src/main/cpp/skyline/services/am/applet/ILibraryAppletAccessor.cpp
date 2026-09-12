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
        LOGI("Applet accessor for {} ID created with appletMode 0x{:X}", ToString(appletId), appletMode);
    }

    ILibraryAppletAccessor::~ILibraryAppletAccessor() {
        indirectLayers->Unregister(indirectLayerHandle);
    }

    Result ILibraryAppletAccessor::StartApplet() {
        LOGI("Library applet Start: id=0x{:X}, mode=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode));
        stateChangeEvent->ResetSignal();
        const auto result{applet->Start()};
        LOGI("Library applet Start completed: id=0x{:X}, mode=0x{:X}, result=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode), static_cast<u32>(result));
        return result;
    }

    bool ILibraryAppletAccessor::IsAppletCompleted() const {
        return stateChangeEvent->signalled;
    }

    Result ILibraryAppletAccessor::GetAppletStateChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        LOGI("Library applet GetAppletStateChangedEvent: id=0x{:X}, handle=0x{:X}",
             static_cast<u32>(appletId), stateChangeEventHandle);
        response.copyHandles.push_back(stateChangeEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(IsAppletCompleted());
        return {};
    }

    Result ILibraryAppletAccessor::Start(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return StartApplet();
    }

    Result ILibraryAppletAccessor::RequestExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        // Strato frontends are in-process rather than independent HOS processes. Marking the
        // state event completes the same observable AM contract without killing the application.
        indirectLayers->Unregister(indirectLayerHandle);
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::Terminate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        indirectLayers->Unregister(indirectLayerHandle);
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
        auto storage{request.PopService<IStorage>(0, session)};
        LOGI("Library applet PushInData: id=0x{:X}, mode=0x{:X}, size=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode),
             storage ? storage->GetSpan().size() : 0);
        applet->PushNormalDataToApplet(std::move(storage));
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
        LOGI("Library applet PushInteractiveInData: id=0x{:X}, mode=0x{:X}, size=0x{:X}, command=0x{:X}",
             static_cast<u32>(appletId), static_cast<u32>(appletMode), storageSize, command);
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
            LOGI("Library applet PopInteractiveOutData: id=0x{:X}, size=0x{:X}",
                 static_cast<u32>(appletId), outStorage->GetSpan().size());
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        LOGI("Library applet PopInteractiveOutData: id=0x{:X}, no data", static_cast<u32>(appletId));
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popNormalOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        LOGI("Library applet GetPopInteractiveOutDataEvent: id=0x{:X}, handle=0x{:X}",
             static_cast<u32>(appletId), popInteractiveOutDataEventHandle);
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (!indirectLayerHandle || !indirectLayers->Get(indirectLayerHandle))
            return result::ObjectInvalid;
        LOGI("Library applet GetIndirectLayerConsumerHandle: id=0x{:X}, handle=0x{:X}",
             static_cast<u32>(appletId), indirectLayerHandle);
        response.Push<u64>(indirectLayerHandle);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(unknown170Event));
        return {};
    }
}
