// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <input.h>
#include "IActiveVibrationDeviceList.h"
#include "results.h"

using namespace skyline::input;

namespace skyline::service::hid {
    IActiveVibrationDeviceList::IActiveVibrationDeviceList(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IActiveVibrationDeviceList::ActivateVibrationDevice(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto handle{request.Pop<NpadDeviceHandle>()};

        if (!NpadManager::IsNpadIdValid(handle.id))
            return result::VibrationInvalidNpadId;
        if (handle.padding != 0)
            return result::InvalidNpadHandle;
        if (!NpadManager::IsVibrationHandleValid(handle))
            return handle.GetType() == NpadControllerType::None ? result::VibrationInvalidStyleIndex : result::VibrationDeviceIndexOutOfRange;

        state.input->npad.at(handle.id).ActivateVibrationDevice(handle);

        return {};
    }
}
