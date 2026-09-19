// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/am/applet/IApplet.h>
#include <services/applet/common_arguments.h>

namespace skyline::applet {
    /**
     * @brief HLE fallback for the Data Erase applet
     *
     * The actual system applet is not emulated, but the frontend contract is preserved:
     * input storages are accepted, output storages are produced, and the corresponding
     * events are signalled before completion.
     */
    class DataEraseApplet : public service::am::IApplet,
                            private service::am::EnableNormalQueue,
                            private service::am::EnableInteractiveQueue {
      public:
        DataEraseApplet(const DeviceState &state, service::ServiceManager &manager, std::shared_ptr<kernel::type::KEvent> onAppletStateChanged, std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet, std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet, service::applet::LibraryAppletMode appletMode);

        Result Start() override;

        Result GetResult() override;

        void PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) override;

        void PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) override;
    };
}
