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
                                               appletMode)) {
        stateChangeEventHandle = state.process->InsertItem(stateChangeEvent);
        popNormalOutDataEventHandle = state.process->InsertItem(popNormalOutDataEvent);
        popInteractiveOutDataEventHandle = state.process->InsertItem(popInteractiveOutDataEvent);
        LOGD("Applet accessor for {} ID created with appletMode 0x{:X}", ToString(appletId), appletMode);
    }

    Result ILibraryAppletAccessor::StartApplet() {
        stateChangeEvent->ResetSignal();
        return applet->Start();
    }

    bool ILibraryAppletAccessor::IsAppletCompleted() const {
        return stateChangeEvent->signalled;
    }

    Result ILibraryAppletAccessor::GetAppletStateChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(stateChangeEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u8>(IsAppletCompleted());
        return {};
    }

    Result ILibraryAppletAccessor::Start(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        LOGD("Applet trace: Start entering for {} (mode=0x{:X}, stateChanged={})", ToString(appletId), appletMode, stateChangeEvent->signalled);
        const Result result{StartApplet()};
        LOGD("Applet trace: Start returned for {} (result=0x{:X}, stateChanged={})", ToString(appletId), result.raw, stateChangeEvent->signalled);
        return result;
    }

    Result ILibraryAppletAccessor::RequestExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        // Strato frontends are in-process rather than independent HOS processes. Marking the
        // state event completes the same observable AM contract without killing the application.
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::Terminate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        stateChangeEvent->Signal();
        return {};
    }

    Result ILibraryAppletAccessor::GetResult(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return applet->GetResult();
    }

    Result ILibraryAppletAccessor::PresetLibraryAppletGpuTimeSliceZero(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::Unknown90(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (request.cmdArgSz < (sizeof(u64) * 4)) {
            LOGD("Applet trace: Unknown90 for {} has only 0x{:X} bytes of command data", ToString(appletId), request.cmdArgSz);
        } else {
            const u64 value0{request.Pop<u64>()};
            const u64 value1{request.Pop<u64>()};
            const u64 value2{request.Pop<u64>()};
            const u64 value3{request.Pop<u64>()};
            LOGD("Applet trace: Unknown90 for {} (args=[0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}])", ToString(appletId), value0, value1, value2, value3);
        }
        return {};
    }

    Result ILibraryAppletAccessor::PushInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        applet->PushNormalDataToApplet(request.PopService<IStorage>(0, session));
        return {};
    }

    Result ILibraryAppletAccessor::PushInteractiveInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        applet->PushInteractiveDataToApplet(request.PopService<IStorage>(0, session));
        return {};
    }

    Result ILibraryAppletAccessor::PopOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (auto outStorage{applet->PopNormalAndClear()}) {
            LOGD("Applet trace: PopOutData for {} returned 0x{:X} bytes", ToString(appletId), outStorage->GetSpan().size());
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        LOGD("Applet trace: PopOutData for {} returned NotAvailable", ToString(appletId));
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
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto appletResourceUserId{request.Pop<u64>()};
        response.Push<u64>(1);
        return {};
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(unknown170Event));
        return {};
    }
}
