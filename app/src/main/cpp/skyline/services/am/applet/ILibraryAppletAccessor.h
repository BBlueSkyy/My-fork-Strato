// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <kernel/types/KEvent.h>
#include <services/serviceman.h>
#include "IApplet.h"
#include <applet/applet_creator.h>

namespace skyline::service::am {
    namespace result {
        constexpr Result ObjectInvalid(128, 500);
        constexpr Result OutOfBounds(128, 503);
        constexpr Result NotAvailable(128, 2);
    }

    class ILibraryAppletAccessor : public BaseService {
      private:
        std::shared_ptr<kernel::type::KEvent> stateChangeEvent;
        std::shared_ptr<kernel::type::KEvent> popNormalOutDataEvent;
        std::shared_ptr<kernel::type::KEvent> popInteractiveOutDataEvent;
        std::shared_ptr<kernel::type::KEvent> unknown170Event;

        KHandle stateChangeEventHandle{};
        KHandle popNormalOutDataEventHandle{};
        KHandle popInteractiveOutDataEventHandle{};

        skyline::applet::AppletId appletId;
        applet::LibraryAppletMode appletMode;
        std::shared_ptr<IApplet> applet;

      public:
        ILibraryAppletAccessor(const DeviceState &state, ServiceManager &manager,
                               skyline::applet::AppletId appletId, applet::LibraryAppletMode appletMode);

        Result StartApplet();
        bool IsAppletCompleted() const;

        Result GetAppletStateChangedEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result IsCompleted(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Start(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result RequestExit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Terminate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetResult(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PresetLibraryAppletGpuTimeSliceZero(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Unknown90(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PushInData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PopOutData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PushInteractiveInData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PopInteractiveOutData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPopOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPopInteractiveOutDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetIndirectLayerConsumerHandle(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result Unknown170(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0, ILibraryAppletAccessor, GetAppletStateChangedEvent),
            SFUNC(1, ILibraryAppletAccessor, IsCompleted),
            SFUNC(10, ILibraryAppletAccessor, Start),
            SFUNC(20, ILibraryAppletAccessor, RequestExit),
            SFUNC(25, ILibraryAppletAccessor, Terminate),
            SFUNC(30, ILibraryAppletAccessor, GetResult),
            SFUNC(60, ILibraryAppletAccessor, PresetLibraryAppletGpuTimeSliceZero),
            SFUNC(90, ILibraryAppletAccessor, Unknown90),
            SFUNC(100, ILibraryAppletAccessor, PushInData),
            SFUNC(101, ILibraryAppletAccessor, PopOutData),
            SFUNC(103, ILibraryAppletAccessor, PushInteractiveInData),
            SFUNC(104, ILibraryAppletAccessor, PopInteractiveOutData),
            SFUNC(105, ILibraryAppletAccessor, GetPopOutDataEvent),
            SFUNC(106, ILibraryAppletAccessor, GetPopInteractiveOutDataEvent),
            SFUNC(120, ILibraryAppletAccessor, GetLibraryAppletInfo),
            SFUNC(160, ILibraryAppletAccessor, GetIndirectLayerConsumerHandle),
            SFUNC(170, ILibraryAppletAccessor, Unknown170)
        )
    };
}
