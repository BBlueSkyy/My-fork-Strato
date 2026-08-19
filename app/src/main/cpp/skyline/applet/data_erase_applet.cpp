// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "data_erase_applet.h"

namespace skyline::applet {
    DataEraseApplet::DataEraseApplet(const DeviceState &state,
                                     service::ServiceManager &manager,
                                     std::shared_ptr<kernel::type::KEvent> onAppletStateChanged,
                                     std::shared_ptr<kernel::type::KEvent> onNormalDataPushFromApplet,
                                     std::shared_ptr<kernel::type::KEvent> onInteractiveDataPushFromApplet,
                                     service::applet::LibraryAppletMode appletMode)
        : IApplet{state, manager, std::move(onAppletStateChanged), std::move(onNormalDataPushFromApplet), std::move(onInteractiveDataPushFromApplet), appletMode} {}

    Result DataEraseApplet::Start() {
        LOGW("DataEraseApplet: Start() called, this is a no-op stub - no save data will be erased");

        // Notify the guest that we've finished running
        onAppletStateChanged->Signal();
        return {};
    }

    Result DataEraseApplet::GetResult() {
        return {};
    }

    void DataEraseApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {}

    void DataEraseApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {}
}
