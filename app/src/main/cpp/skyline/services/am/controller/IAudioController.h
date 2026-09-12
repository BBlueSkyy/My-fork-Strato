// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::am {
    class IAudioController : public BaseService {
      private:
        float mainAppletVolume{1.0F};
        float libraryAppletVolume{1.0F};
        float transparentVolumeRate{1.0F};
        i64 fadeTimeNs{};

      public:
        IAudioController(const DeviceState &state, ServiceManager &manager);

        Result SetExpectedMasterVolume(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetMainAppletExpectedMasterVolume(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result GetLibraryAppletExpectedMasterVolume(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result ChangeMainAppletMasterVolume(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);
        Result SetTransparentVolumeRate(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &);

        SERVICE_DECL(
            SFUNC(0, IAudioController, SetExpectedMasterVolume),
            SFUNC(1, IAudioController, GetMainAppletExpectedMasterVolume),
            SFUNC(2, IAudioController, GetLibraryAppletExpectedMasterVolume),
            SFUNC(3, IAudioController, ChangeMainAppletMasterVolume),
            SFUNC(4, IAudioController, SetTransparentVolumeRate)
        )
    };
}
