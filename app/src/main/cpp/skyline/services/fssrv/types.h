// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <services/account/IAccountServiceForApplication.h>

namespace skyline::service::fssrv {
    enum class SaveDataSpaceId : u8 {
        System = 0,
        User = 1,
        SdSystem = 2,
        Temporary = 3,
        SdCache = 4,
        ProperSystem = 100,
    };

    enum class SaveDataType : u8 {
        System = 0,
        Account = 1,
        Bcat = 2,
        Device = 3,
        Temporary = 4,
        Cache = 5,
        SystemBcat = 6,
    };

    enum class SaveDataRank : u8 {
        Primary = 0,
        Secondary = 1,
    };

    struct SaveDataAttribute {
        u64 programId;
        account::UserId userId;
        u64 saveDataId;
        SaveDataType type;
        SaveDataRank rank;
        u16 index;
        u8 padding[0x1A];
    };
    static_assert(sizeof(SaveDataAttribute) == 0x40);

    struct SaveDataInfo {
        u64 saveDataId;
        SaveDataSpaceId spaceId;
        SaveDataType type;
        u8 padding0[6];
        account::UserId userId;
        u64 systemSaveDataId;
        u64 applicationId;
        u64 size;
        u16 index;
        SaveDataRank rank;
        u8 padding1[0x25];
    };
    static_assert(sizeof(SaveDataInfo) == 0x60);

    struct FileTimeStampRaw {
        u64 created;
        u64 modified;
        u64 accessed;
        u8 isValid;
        u8 padding[7];
    };
    static_assert(sizeof(FileTimeStampRaw) == 0x20);

    struct FileSystemAttribute {
        bool directoryNameLengthMaxHasValue;
        bool fileNameLengthMaxHasValue;
        bool directoryPathLengthMaxHasValue;
        bool filePathLengthMaxHasValue;
        bool utf16CreateDirectoryPathLengthMaxHasValue;
        bool utf16DeleteDirectoryPathLengthMaxHasValue;
        bool utf16RenameSourceDirectoryPathLengthMaxHasValue;
        bool utf16RenameDestinationDirectoryPathLengthMaxHasValue;
        bool utf16OpenDirectoryPathLengthMaxHasValue;
        bool utf16DirectoryNameLengthMaxHasValue;
        bool utf16FileNameLengthMaxHasValue;
        bool utf16DirectoryPathLengthMaxHasValue;
        bool utf16FilePathLengthMaxHasValue;
        u8 reserved1[0x1B];
        i32 directoryNameLengthMax;
        i32 fileNameLengthMax;
        i32 directoryPathLengthMax;
        i32 filePathLengthMax;
        i32 utf16CreateDirectoryPathLengthMax;
        i32 utf16DeleteDirectoryPathLengthMax;
        i32 utf16RenameSourceDirectoryPathLengthMax;
        i32 utf16RenameDestinationDirectoryPathLengthMax;
        i32 utf16OpenDirectoryPathLengthMax;
        i32 utf16DirectoryNameLengthMax;
        i32 utf16FileNameLengthMax;
        i32 utf16DirectoryPathLengthMax;
        i32 utf16FilePathLengthMax;
        u8 reserved2[0x64];
    };
    static_assert(sizeof(FileSystemAttribute) == 0xC0);
}
