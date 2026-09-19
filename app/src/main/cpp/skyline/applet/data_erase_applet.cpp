// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <services/am/storage/VectorIStorage.h>

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
        LOGW("DataEraseApplet: using frontend stub completion path");

        // Match the generic DataErase frontend fallback used by Yuzu/Eden: consume
        // all applet inputs, publish normal and interactive outputs, then complete.
        {
            std::scoped_lock lock{normalInputDataMutex};
            while (!normalInputData.empty())
                normalInputData.pop();
        }
        {
            std::scoped_lock lock{interactiveInputDataMutex};
            while (!interactiveInputData.empty())
                interactiveInputData.pop();
        }

        PushNormalDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, 0x1000));
        PushInteractiveDataAndSignal(std::make_shared<service::am::VectorIStorage>(state, manager, 0x1000));

        onAppletStateChanged->Signal();
        return {};
    }

    Result DataEraseApplet::GetResult() {
        return {};
    }

    void DataEraseApplet::PushNormalDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushNormalInput(std::move(data));
    }

    void DataEraseApplet::PushInteractiveDataToApplet(std::shared_ptr<service::am::IStorage> data) {
        PushInteractiveInput(std::move(data));
    }
}
