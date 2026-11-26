// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>
#include "types.h"

namespace skyline::service::fssrv {

    /**
     * @url https://switchbrew.org/wiki/Filesystem_services#ISaveDataInfoReader
     */
    class ISaveDataInfoReader : public BaseService {
      private:
        std::vector<SaveDataInfo> entries;
        size_t cursor{};

      public:
        ISaveDataInfoReader(const DeviceState &state, ServiceManager &manager, std::vector<SaveDataInfo> entries = {}, std::optional<SaveDataSpaceId> spaceFilter = std::nullopt, bool onlyCache = false);

        /**
         * @brief Reads a batch of save data info entries into the supplied output buffer
         * @url https://switchbrew.org/wiki/Filesystem_services#ReadSaveDataInfo
         */
        Result ReadSaveDataInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, ISaveDataInfoReader, ReadSaveDataInfo)
        )
    };
}
