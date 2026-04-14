// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IShopServiceAsync.h"

namespace skyline::service::nim {
    IShopServiceAsync::IShopServiceAsync(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IShopServiceAsync::Cancel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IShopServiceAsync::GetSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(0);
        return {};
    }

    Result IShopServiceAsync::Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IShopServiceAsync::GetErrorCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u32>(0);
        return {};
    }

    Result IShopServiceAsync::Request(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IShopServiceAsync::Prepare(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }
}
