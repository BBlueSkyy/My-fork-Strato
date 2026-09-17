// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstring>
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
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] accessor created mode=0x{:X}", static_cast<u32>(appletMode));
    }

    Result ILibraryAppletAccessor::StartApplet() {
        stateChangeEvent->ResetSignal();
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] StartApplet mode=0x{:X}", static_cast<u32>(appletMode));
        auto result{applet->Start()};
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] StartApplet returned result=0x{:X}", result.raw);
        return result;
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
        return StartApplet();
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

    Result ILibraryAppletAccessor::Unknown90(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return {};
    }

    Result ILibraryAppletAccessor::PushInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] PushInData");
        applet->PushNormalDataToApplet(request.PopService<IStorage>(0, session));
        return {};
    }

    Result ILibraryAppletAccessor::PushInteractiveInData(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto storage{request.PopService<IStorage>(0, session)};
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd) {
            const auto data{storage ? storage->GetSpan() : span<u8>{}};
            u32 command{};
            if (data.size() >= sizeof(command))
                std::memcpy(&command, data.data(), sizeof(command));

            LOGI("[SWKBD-TRACE] PushInteractiveInData size=0x{:X} command=0x{:X}", data.size(), command);

            if (command == 0xA && data.size() >= 0x1C) {
                u16 calcArgSize{};
                u64 flags{};
                std::memcpy(&calcArgSize, data.data() + 0x8, sizeof(calcArgSize));
                std::memcpy(&flags, data.data() + 0xC, sizeof(flags));
                LOGI("[SWKBD-TRACE] Calc header calcArgSize=0x{:X} flags=0x{:X}", calcArgSize, flags);
            }
        }

        applet->PushInteractiveDataToApplet(std::move(storage));

        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] PushInteractiveInData returned");
        return {};
    }

    Result ILibraryAppletAccessor::PopOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (auto outStorage{applet->PopNormalAndClear()}) {
            if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
                LOGI("[SWKBD-TRACE] PopOutData -> storage");
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] PopOutData -> NotAvailable");
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::PopInteractiveOutData(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (auto outStorage{applet->PopInteractiveAndClear()}) {
            if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
                LOGI("[SWKBD-TRACE] PopInteractiveOutData -> storage");
            manager.RegisterService(outStorage, session, response);
            return {};
        }
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] PopInteractiveOutData -> NotAvailable");
        return result::NotAvailable;
    }

    Result ILibraryAppletAccessor::GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(popNormalOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd)
            LOGI("[SWKBD-TRACE] GetPopInteractiveOutDataEvent");
        response.copyHandles.push_back(popInteractiveOutDataEventHandle);
        return {};
    }

    Result ILibraryAppletAccessor::GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(static_cast<u32>(appletId));
        response.Push<u32>(static_cast<u32>(appletMode));
        return {};
    }

    Result ILibraryAppletAccessor::GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (appletId == skyline::applet::AppletId::LibraryAppletSwkbd) {
            // Eden's custom SWKBD frontend only requires a non-zero token here and does not use an indirect display handle.
            LOGI("[SWKBD-TRACE] GetIndirectLayerConsumerHandle -> 0xDEADBEEF");
            response.Push<u64>(0xDEADBEEFULL);
            return {};
        }
        return result::ObjectInvalid;
    }

    Result ILibraryAppletAccessor::Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.copyHandles.push_back(state.process->InsertItem(unknown170Event));
        return {};
    }
}
