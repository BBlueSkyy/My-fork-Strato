// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <services/serviceman.h>
#include <services/am/applet_state.h>

namespace skyline::service::am {
    class NativeAppletContext;

    class ILibraryAppletSelfAccessor : public BaseService {
      private:
        std::shared_ptr<AppletState> appletState;
        std::shared_ptr<NativeAppletContext> context;

        struct AppletIdentityInfo {
            u32 appletId;
            u32 padding{};
            u64 applicationId;
        };
        static_assert(sizeof(AppletIdentityInfo) == 0x10);

        Result PopInData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PushOutData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PopInteractiveInData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result PushInteractiveOutData(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPopInDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetPopInteractiveInDataEvent(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ExitProcessAndReturn(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLibraryAppletInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetMainAppletIdentityInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result CanUseApplicationCore(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetCallerAppletIdentityInfo(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetDesirableKeyboardLayout(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

      public:
        ILibraryAppletSelfAccessor(const DeviceState &state, ServiceManager &manager,
                                   std::shared_ptr<AppletState> appletState);

        SERVICE_DECL(
            SFUNC(0, ILibraryAppletSelfAccessor, PopInData),
            SFUNC(1, ILibraryAppletSelfAccessor, PushOutData),
            SFUNC(2, ILibraryAppletSelfAccessor, PopInteractiveInData),
            SFUNC(3, ILibraryAppletSelfAccessor, PushInteractiveOutData),
            SFUNC(5, ILibraryAppletSelfAccessor, GetPopInDataEvent),
            SFUNC(6, ILibraryAppletSelfAccessor, GetPopInteractiveInDataEvent),
            SFUNC(10, ILibraryAppletSelfAccessor, ExitProcessAndReturn),
            SFUNC(11, ILibraryAppletSelfAccessor, GetLibraryAppletInfo),
            SFUNC(12, ILibraryAppletSelfAccessor, GetMainAppletIdentityInfo),
            SFUNC(13, ILibraryAppletSelfAccessor, CanUseApplicationCore),
            SFUNC(14, ILibraryAppletSelfAccessor, GetCallerAppletIdentityInfo),
            SFUNC(19, ILibraryAppletSelfAccessor, GetDesirableKeyboardLayout)
        )
    };
}
