// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <os.h>
#include <cstring>
#include <vfs/os_filesystem.h>
#include <vfs/nca.h>
#include <loader/loader.h>
#include "results.h"
#include "IStorage.h"
#include "IMultiCommitManager.h"
#include "IFileSystemProxy.h"
#include "ISaveDataInfoReader.h"
#include "validation.h"

namespace skyline::service::fssrv {
    namespace {
        struct OpenSaveDataInput {
            SaveDataSpaceId spaceId;
            u8 padding[7];
            SaveDataAttribute attribute;
        };
        static_assert(sizeof(OpenSaveDataInput) == 0x48);

        struct OpenDataStorageInput {
            StorageId storageId;
            u8 padding[7];
            u64 dataId;
        };
        static_assert(sizeof(OpenDataStorageInput) == 0x10);

        template<typename T>
        std::optional<T> ReadArgument(const ipc::IpcRequest &request) {
            if (!request.cmdArg || request.cmdArgSz < sizeof(T))
                return std::nullopt;
            T value{};
            std::memcpy(&value, request.cmdArg, sizeof(T));
            return value;
        }

        bool IsValidStorageId(StorageId storageId) {
            switch (storageId) {
                case StorageId::Host:
                case StorageId::GameCard:
                case StorageId::NandSystem:
                case StorageId::NandUser:
                case StorageId::SdCard:
                    return true;
                default:
                    return false;
            }
        }
    }

    std::optional<std::string> GetSaveDataPath(SaveDataSpaceId spaceId, SaveDataAttribute attribute, u64 defaultProgramId) {
        if (!IsValidSaveDataSpaceId(spaceId) || !IsValidSaveDataType(attribute.type))
            return std::nullopt;
        if (attribute.programId == 0)
            attribute.programId = defaultProgramId;

        std::string spaceIdStr;
        switch (spaceId) {
            case SaveDataSpaceId::System:
                spaceIdStr = "/nand/system";
                break;
            case SaveDataSpaceId::User:
                spaceIdStr = "/nand/user";
                break;
            case SaveDataSpaceId::Temporary:
                spaceIdStr = "/nand/temp";
                break;
            default:
                return std::nullopt;
        }

        switch (attribute.type) {
            case SaveDataType::System:
                if (spaceId != SaveDataSpaceId::System)
                    return std::nullopt;
                return fmt::format("{}/save/{:016X}/{:016X}{:016X}/", spaceIdStr, attribute.saveDataId, attribute.userId.lower, attribute.userId.upper);
            case SaveDataType::Account:
            case SaveDataType::Device:
                if (spaceId != SaveDataSpaceId::User)
                    return std::nullopt;
                return fmt::format("{}/save/{:016X}/{:016X}{:016X}/{:016X}/", spaceIdStr, 0, attribute.userId.lower, attribute.userId.upper, attribute.programId);
            case SaveDataType::Temporary:
                if (spaceId != SaveDataSpaceId::Temporary)
                    return std::nullopt;
                return fmt::format("{}/{:016X}/{:016X}{:016X}/{:016X}/", spaceIdStr, 0, attribute.userId.lower, attribute.userId.upper, attribute.programId);
            case SaveDataType::Cache:
                if (spaceId != SaveDataSpaceId::User)
                    return std::nullopt;
                return fmt::format("{}/save/cache/{:016X}/", spaceIdStr, attribute.programId);
            default:
                return std::nullopt;
        }
    }

    IFileSystemProxy::IFileSystemProxy(const DeviceState &state, ServiceManager &manager) : BaseService(state, manager) {}

    Result IFileSystemProxy::SetCurrentProcess(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        process = request.pid;
        return {};
    }

    Result IFileSystemProxy::OpenSdCardFileSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(std::make_shared<IFileSystem>(std::make_shared<vfs::OsFileSystem>(state.os->publicAppFilesPath + "/switch/sdmc/"), state, manager), session, response);
        return {};
    }

    Result IFileSystemProxy::GetCacheStorageSize(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (!ReadArgument<u16>(request))
            return result::InvalidArgument;
        return result::NotImplemented;
    }

    Result IFileSystemProxy::OpenSaveDataFileSystemImpl(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response, bool readOnly) {
        const auto input{ReadArgument<OpenSaveDataInput>(request)};
        if (!input)
            return result::InvalidArgument;
        if (!IsValidSaveDataSpaceId(input->spaceId) || !IsValidSaveDataType(input->attribute.type) || !IsValidSaveDataRank(input->attribute.rank))
            return result::InvalidArgument;
        if (input->attribute.rank != SaveDataRank::Primary || input->attribute.index != 0)
            return result::NotImplemented;
        if (input->attribute.programId == 0 && (!state.loader || !state.loader->nacp))
            return result::EntityNotFound;

        const u64 defaultProgramId{input->attribute.programId == 0 ? state.loader->nacp->nacpContents.saveDataOwnerId : 0};
        const auto saveDataPath{GetSaveDataPath(input->spaceId, input->attribute, defaultProgramId)};
        if (!saveDataPath)
            return result::NotImplemented;

        auto fileSystem{std::make_shared<vfs::OsFileSystem>(state.os->publicAppFilesPath + "/switch" + *saveDataPath)};
        manager.RegisterService(std::make_shared<IFileSystem>(std::move(fileSystem), state, manager, readOnly), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenSaveDataFileSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return OpenSaveDataFileSystemImpl(session, request, response, false);
    }

    Result IFileSystemProxy::OpenReadOnlySaveDataFileSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return OpenSaveDataFileSystemImpl(session, request, response, true);
    }

    Result IFileSystemProxy::OpenSaveDataInfoReader(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(std::make_shared<ISaveDataInfoReader>(state, manager), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenSaveDataInfoReaderBySaveDataSpaceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto spaceId{ReadArgument<SaveDataSpaceId>(request)};
        if (!spaceId || !IsValidSaveDataSpaceId(*spaceId))
            return result::InvalidArgument;
        manager.RegisterService(std::make_shared<ISaveDataInfoReader>(state, manager, std::vector<SaveDataInfo>{}, *spaceId), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenSaveDataInfoReaderOnlyCacheStorage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(std::make_shared<ISaveDataInfoReader>(state, manager, std::vector<SaveDataInfo>{}, std::nullopt, true), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenDataStorageByCurrentProcess(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (!state.loader)
            return result::NoRomFsAvailable;
        auto backing{state.loader->currentProcessRomFs};
        if (!backing)
            return result::NoRomFsAvailable;
        LOGI("OpenDataStorageByCurrentProcess: resolved Program storage, {}", state.loader->currentProcessRomFsIdentity);
        manager.RegisterService(std::make_shared<IStorage>(backing, state, manager), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenDataStorageByDataId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto input{ReadArgument<OpenDataStorageInput>(request)};
        if (!input || !IsValidStorageId(input->storageId))
            return result::InvalidArgument;
        if (input->storageId == StorageId::Host)
            return result::NotImplemented;

        // DLC content has its own RomFS; it is never patched against the current Program NCA.
        if (input->storageId != StorageId::NandSystem) {
            for (const auto &dlc : state.dlcLoaders) {
                if (!dlc || !dlc->cnmt || dlc->cnmt->header.id != input->dataId)
                    continue;
                auto romFs{dlc->publicNca ? dlc->publicNca->romFs : nullptr};
                if (!romFs)
                    return result::EntityNotFound;
                manager.RegisterService(std::make_shared<IStorage>(romFs, state, manager), session, response);
                return {};
            }
            return result::EntityNotFound;
        }

        bool archiveError{};
        try {
            auto systemArchivesFileSystem{std::make_shared<vfs::OsFileSystem>(state.os->publicAppFilesPath + "/switch/nand/system/Contents/registered/")};
            auto systemArchives{systemArchivesFileSystem->OpenDirectory("")};
            auto keyStore{std::make_shared<skyline::crypto::KeyStore>(state.os->privateAppFilesPath + "keys")};

            for (const auto &entry : systemArchives->Read()) {
                if (entry.type != vfs::Directory::EntryType::File)
                    continue;
                auto backing{systemArchivesFileSystem->OpenFileUnchecked(entry.name)};
                if (!backing)
                    continue;
                try {
                    auto nca{vfs::NCA(backing, keyStore)};
                    if (nca.header.titleId == input->dataId && nca.romFs != nullptr) {
                        manager.RegisterService(std::make_shared<IStorage>(nca.romFs, state, manager), session, response);
                        return {};
                    }
                } catch (const std::exception &) {
                    archiveError = true;
                }
            }
        } catch (const std::exception &) {
            archiveError = true;
        }

        if (!state.os->assetFileSystem)
            return archiveError ? result::UnexpectedFailure : result::EntityNotFound;
        auto assetBacking{state.os->assetFileSystem->OpenFileUnchecked(fmt::format("romfs/{:016X}", input->dataId))};
        if (!assetBacking)
            return archiveError ? result::UnexpectedFailure : result::EntityNotFound;
        manager.RegisterService(std::make_shared<IStorage>(std::move(assetBacking), state, manager), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenPatchDataStorageByCurrentProcess(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // A base-only title (or an ExeFS-only update) has no Program patch data to open.
        // When available, this is the same persistent base+patch view used by command 200.
        if (!state.loader)
            return result::EntityNotFound;
        auto backing{state.loader->patchDataRomFs};
        if (!backing)
            return result::EntityNotFound;
        LOGI("OpenPatchDataStorageByCurrentProcess: resolved Program patch storage, {}", state.loader->currentProcessRomFsIdentity);
        manager.RegisterService(std::make_shared<IStorage>(backing, state, manager), session, response);
        return {};
    }

    Result IFileSystemProxy::SetGlobalAccessLogMode(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto mode{ReadArgument<u32>(request)};
        if (!mode || *mode > 2)
            return result::InvalidArgument;
        globalAccessLogMode = *mode;
        return {};
    }

    Result IFileSystemProxy::GetGlobalAccessLogMode(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(globalAccessLogMode);
        return {};
    }

    Result IFileSystemProxy::OpenMultiCommitManager(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IMultiCommitManager), session, response);
        return {};
    }
}
