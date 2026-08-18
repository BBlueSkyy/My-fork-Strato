// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IFileSystemProxy.h"
#include "ISaveDataInfoReader.h"

namespace skyline::service::fssrv {
    /**
     * @brief nn::fs::SaveDataInfo, the 0x60-byte struct written into the output buffer of ReadSaveDataInfo
     * @url https://switchbrew.org/wiki/Filesystem_services#SaveDataInfo
     */
    struct __attribute__((packed)) SaveDataInfo {
        u64 saveDataId; //!< The ID of the savedata
        u8 spaceId; //!< The storage location of the savedata, matches SaveDataSpaceId but is only 1 byte on the wire (unlike Skyline's u64-backed enum)
        SaveDataType type; //!< The type of savedata
        u8 _pad0_[6];
        account::UserId userId; //!< The user ID the savedata belongs to
        u64 systemSaveDataId; //!< The system savedata ID, 0 for account savedata
        u64 applicationId; //!< The application ID the savedata belongs to
        u64 size; //!< The raw size of the savedata image
        u16 index; //!< The index of the savedata
        SaveDataRank rank; //!< The rank of the savedata
        u8 _pad1_[0x25];
    };
    static_assert(sizeof(SaveDataInfo) == 0x60);

    ISaveDataInfoReader::ISaveDataInfoReader(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result ISaveDataInfoReader::ReadSaveDataInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Every caller that opens an ISaveDataInfoReader (OpenSaveDataInfoReader,
        // OpenSaveDataInfoReaderBySaveDataSpaceId, OpenSaveDataInfoReaderOnlyCacheStorage)
        // is expected to loop this command until it reports 0 entries, at which point
        // iteration stops cleanly. Since we don't have any entries to report, we can
        // safely report 0 on the first call without touching the output buffer at all
        response.Push<i64>(0);
        return {};
    }
}
