// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline::service::fssrv {

    /**
     * @url https://switchbrew.org/wiki/Filesystem_services#ISaveDataInfoReader
     */
    class ISaveDataInfoReader : public BaseService {
      public:
        ISaveDataInfoReader(const DeviceState &state, ServiceManager &manager);

        /**
         * @brief Reads a batch of save data info entries into the supplied output buffer
         * @url https://switchbrew.org/wiki/Filesystem_services#ReadSaveDataInfo
         * @note Skyline does not currently track a save data info database so this always
         * reports 0 entries, this is a valid response and is the documented way for callers
         * to detect there are no (more) entries to read rather than being an error condition
         */
        Result ReadSaveDataInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

        SERVICE_DECL(
            SFUNC(0x0, ISaveDataInfoReader, ReadSaveDataInfo)
        )
    };
}
