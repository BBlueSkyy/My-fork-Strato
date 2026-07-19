// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::nim {
    /**
     * @brief IShopServiceAsync is used by applications to directly communicate with the Nintendo eShop
     * @url https://switchbrew.org/wiki/NIM_services#IShopServiceAsync
     */
    class IShopServiceAsync : public BaseService {
      public:
        IShopServiceAsync(const DeviceState &state, ServiceManager &manager);

        Result Cancel(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Read(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetErrorCode(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Request(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Prepare(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IShopServiceAsync, Cancel),
            SFUNC(0x1, IShopServiceAsync, GetSize),
            SFUNC(0x2, IShopServiceAsync, Read),
            SFUNC(0x3, IShopServiceAsync, GetErrorCode),
            SFUNC(0x4, IShopServiceAsync, Request),
            SFUNC(0x5, IShopServiceAsync, Prepare)
        )
    };
}
