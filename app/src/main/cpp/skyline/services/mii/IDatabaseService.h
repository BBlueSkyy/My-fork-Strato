// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "mii_database.h"

namespace skyline::service::mii {
    /**
     * @url https://switchbrew.org/wiki/Shared_Database_services#IDatabaseService
     */
    class IDatabaseService : public BaseService {
      private:
        MiiDatabase &database;
        [[maybe_unused]] u32 databaseType{};
        u32 interfaceVersion{};
        u64 updateCounter{};

      public:
        IDatabaseService(const DeviceState &state, ServiceManager &manager, MiiDatabase &database, u32 databaseType = 0);

        Result IsUpdated(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result IsFullDatabase(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Get(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Get1(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result UpdateLatest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result BuildRandom(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result BuildDefault(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result GetIndex(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result SetInterfaceVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result DeleteFile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        Result Append(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, IDatabaseService, IsUpdated),
            SFUNC(0x1, IDatabaseService, IsFullDatabase),
            SFUNC(0x2, IDatabaseService, GetCount),
            SFUNC(0x3, IDatabaseService, Get),
            SFUNC(0x4, IDatabaseService, Get1),
            SFUNC(0x5, IDatabaseService, UpdateLatest),
            SFUNC(0x6, IDatabaseService, BuildRandom),
            SFUNC(0x7, IDatabaseService, BuildDefault),
            SFUNC(0x10, IDatabaseService, DeleteFile),
            SFUNC(0x15, IDatabaseService, GetIndex),
            SFUNC(0x16, IDatabaseService, SetInterfaceVersion),
            SFUNC(0x1A, IDatabaseService, Append)
        )
    };
}
