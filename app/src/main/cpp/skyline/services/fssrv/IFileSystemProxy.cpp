// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <os.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vfs/os_filesystem.h>
#include <vfs/nca.h>
#include <loader/loader.h>
#include "results.h"
#include "IStorage.h"
#include "IMultiCommitManager.h"
#include "IFileSystemProxy.h"
#include "ISaveDataInfoReader.h"
#include "helpers.h"
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

        constexpr u32 CacheStorageMetadataMagic{0x43414348};
        constexpr u32 CacheStorageMetadataVersion{2};

        struct CacheStorageMetadata {
            u32 magic{CacheStorageMetadataMagic};
            u32 version{CacheStorageMetadataVersion};
            u64 saveDataOwnerId{};
            u64 saveDataId{};
            u64 dataSize{};
            u64 journalSize{};
            u64 logicalSize{};
            u16 index{};
            std::array<u8, 6> reserved{};
        };
        static_assert(sizeof(CacheStorageMetadata) == 0x38);

        struct CacheStorageMetadataV1 {
            u32 magic{};
            u32 version{};
            u64 saveDataOwnerId{};
            u64 dataSize{};
            u64 journalSize{};
            u16 index{};
            std::array<u8, 6> reserved{};
        };
        static_assert(sizeof(CacheStorageMetadataV1) == 0x28);

        u64 MakeCacheSaveDataId(u64 saveDataOwnerId, u16 index) {
            // Strato has no Horizon save-data indexer. Assign a stable surrogate ID
            // from the durable cache key instead of fabricating one while enumerating.
            constexpr u64 FnvOffset{14695981039346656037ULL};
            constexpr u64 FnvPrime{1099511628211ULL};
            u64 hash{FnvOffset};
            const auto mixByte{[&](u8 value) {
                hash ^= value;
                hash *= FnvPrime;
            }};
            constexpr std::array<u8, 5> Domain{'c', 'a', 'c', 'h', 'e'};
            for (u8 value : Domain)
                mixByte(value);
            for (size_t shift{}; shift < sizeof(saveDataOwnerId) * 8; shift += 8)
                mixByte(static_cast<u8>(saveDataOwnerId >> shift));
            mixByte(static_cast<u8>(index));
            mixByte(static_cast<u8>(index >> 8));
            return hash == 0 ? 1 : hash;
        }

        std::string GetCacheStorageMetadataPath(u64 saveDataOwnerId, u16 index) {
            return fmt::format("/.strato/cache/{:016X}-{:04X}.bin", saveDataOwnerId, index);
        }

        Result ReadCacheStorageMetadata(const std::string &publicAppFilesPath, u64 saveDataOwnerId, u16 index,
                                        std::optional<CacheStorageMetadata> &metadata) {
            metadata.reset();
            try {
                vfs::OsFileSystem root{publicAppFilesPath + "/switch"};
                auto [backing, error]{root.OpenFileWithError(GetCacheStorageMetadataPath(saveDataOwnerId, index))};
                if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
                    return {};
                if (error)
                    return MapVfsError(error);
                if (!backing)
                    return result::UnexpectedFailure;

                if (backing->size == sizeof(CacheStorageMetadataV1)) {
                    CacheStorageMetadataV1 legacy{};
                    auto bytes{span(reinterpret_cast<u8 *>(&legacy), sizeof(legacy))};
                    auto [read, readError]{backing->ReadWithError(bytes)};
                    if (readError)
                        return MapBackingError(readError);
                    if (read != bytes.size())
                        return result::UnexpectedFailure;
                    if (legacy.magic != CacheStorageMetadataMagic ||
                        legacy.version != 1 ||
                        legacy.saveDataOwnerId != saveDataOwnerId ||
                        legacy.index != index ||
                        legacy.dataSize > std::numeric_limits<u64>::max() - legacy.journalSize)
                        return result::UnexpectedFailure;

                    CacheStorageMetadata upgraded{};
                    upgraded.saveDataOwnerId = legacy.saveDataOwnerId;
                    upgraded.saveDataId = MakeCacheSaveDataId(legacy.saveDataOwnerId, legacy.index);
                    upgraded.dataSize = legacy.dataSize;
                    upgraded.journalSize = legacy.journalSize;
                    upgraded.logicalSize = legacy.dataSize + legacy.journalSize;
                    upgraded.index = legacy.index;
                    metadata = upgraded;
                    return {};
                }

                if (backing->size != sizeof(CacheStorageMetadata))
                    return result::UnexpectedFailure;

                CacheStorageMetadata candidate{};
                auto bytes{span(reinterpret_cast<u8 *>(&candidate), sizeof(candidate))};
                auto [read, readError]{backing->ReadWithError(bytes)};
                if (readError)
                    return MapBackingError(readError);
                if (read != bytes.size())
                    return result::UnexpectedFailure;
                if (candidate.magic != CacheStorageMetadataMagic ||
                    candidate.version != CacheStorageMetadataVersion ||
                    candidate.saveDataOwnerId != saveDataOwnerId ||
                    candidate.index != index)
                    return result::UnexpectedFailure;

                metadata = candidate;
                return {};
            } catch (const std::exception &) {
                return result::UnexpectedFailure;
            }
        }

        Result WriteCacheStorageMetadata(const std::string &publicAppFilesPath, u64 saveDataOwnerId, u16 index,
                                         u64 dataSize, u64 journalSize) {
            if (dataSize > static_cast<u64>(std::numeric_limits<i64>::max()) ||
                journalSize > static_cast<u64>(std::numeric_limits<i64>::max()) ||
                dataSize > std::numeric_limits<u64>::max() - journalSize)
                return result::InvalidArgument;
            const u64 logicalSize{dataSize + journalSize};
            if (logicalSize > static_cast<u64>(std::numeric_limits<i64>::max()))
                return result::InvalidArgument;

            try {
                vfs::OsFileSystem root{publicAppFilesPath + "/switch"};
                constexpr const char *MetadataDirectory{"/.strato/cache"};
                const auto directoryError{root.CreateDirectory(MetadataDirectory, true)};
                if (directoryError == std::errc::file_exists) {
                    if (!root.DirectoryExists(MetadataDirectory))
                        return result::PathAlreadyExists;
                } else if (directoryError) {
                    return MapVfsError(directoryError);
                }

                const auto metadataPath{GetCacheStorageMetadataPath(saveDataOwnerId, index)};
                const auto createError{root.CreateFile(metadataPath, sizeof(CacheStorageMetadata))};
                if (createError == std::errc::file_exists) {
                    if (!root.FileExists(metadataPath))
                        return result::PathAlreadyExists;
                } else if (createError) {
                    return MapVfsError(createError);
                }

                auto [backing, openError]{root.OpenFileWithError(metadataPath, {true, true, false})};
                if (openError)
                    return MapVfsError(openError);
                if (!backing)
                    return result::UnexpectedFailure;
                if (backing->size != sizeof(CacheStorageMetadata)) {
                    const auto resizeError{backing->ResizeWithError(sizeof(CacheStorageMetadata))};
                    if (resizeError)
                        return MapBackingError(resizeError, true);
                }

                CacheStorageMetadata metadata{};
                metadata.saveDataOwnerId = saveDataOwnerId;
                metadata.saveDataId = MakeCacheSaveDataId(saveDataOwnerId, index);
                metadata.dataSize = dataSize;
                metadata.journalSize = journalSize;
                metadata.logicalSize = logicalSize;
                metadata.index = index;

                auto bytes{span(reinterpret_cast<u8 *>(&metadata), sizeof(metadata))};
                auto [written, writeError]{backing->WriteWithError(bytes)};
                if (writeError)
                    return MapBackingError(writeError, true);
                if (written != bytes.size())
                    return result::UnexpectedFailure;
                const auto flushError{backing->Flush()};
                if (flushError)
                    return MapBackingError(flushError, true);
                return {};
            } catch (const std::exception &) {
                return result::UnexpectedFailure;
            }
        }

        u64 GetCurrentApplicationId(const DeviceState &state, u64 saveDataOwnerId) {
            if (state.loader && state.loader->cnmt)
                return state.loader->cnmt->header.id;
            return saveDataOwnerId;
        }

        Result GetCacheSaveDataInfo(const std::string &publicAppFilesPath, u64 saveDataOwnerId,
                                    u64 applicationId, std::optional<SaveDataInfo> &info) {
            info.reset();

            std::optional<CacheStorageMetadata> metadata;
            const auto metadataResult{ReadCacheStorageMetadata(publicAppFilesPath, saveDataOwnerId, 0, metadata)};
            if (metadataResult)
                return metadataResult;
            if (!metadata)
                return {};

            SaveDataAttribute attribute{};
            attribute.programId = saveDataOwnerId;
            attribute.type = SaveDataType::Cache;
            attribute.rank = SaveDataRank::Primary;
            attribute.index = metadata->index;
            const auto saveDataPath{GetSaveDataPath(SaveDataSpaceId::User, attribute, saveDataOwnerId)};
            if (!saveDataPath)
                return result::InvalidArgument;

            try {
                vfs::OsFileSystem root{publicAppFilesPath + "/switch"};
                if (!root.DirectoryExists(*saveDataPath))
                    return {};
            } catch (const std::exception &) {
                return result::UnexpectedFailure;
            }

            SaveDataInfo entry{};
            entry.saveDataId = metadata->saveDataId;
            entry.spaceId = SaveDataSpaceId::User;
            entry.type = SaveDataType::Cache;
            entry.applicationId = applicationId;
            entry.size = metadata->logicalSize;
            entry.index = metadata->index;
            entry.rank = SaveDataRank::Primary;
            info = entry;
            return {};
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

    Result CreateSaveDataDirectory(const std::string &publicAppFilesPath, SaveDataSpaceId spaceId,
                                   SaveDataAttribute attribute, u64 defaultProgramId, bool allowExisting) {
        const auto saveDataPath{GetSaveDataPath(spaceId, attribute, defaultProgramId)};
        if (!saveDataPath)
            return result::InvalidArgument;

        try {
            vfs::OsFileSystem root{publicAppFilesPath + "/switch"};
            const auto error{root.CreateDirectory(*saveDataPath, true)};
            if (error == std::errc::file_exists) {
                if (!root.DirectoryExists(*saveDataPath))
                    return result::PathAlreadyExists;
                return allowExisting ? Result{} : result::AlreadyExists;
            }
            return MapVfsError(error);
        } catch (const std::exception &) {
            return result::UnexpectedFailure;
        }
    }

    Result EnsureApplicationSaveData(const std::string &publicAppFilesPath, u64 saveDataOwnerId,
                                     account::UserId userId, u64 accountSaveDataSize, u64 accountJournalSize,
                                     u64 deviceSaveDataSize, u64 deviceJournalSize) {
        if ((accountSaveDataSize > 0 || accountJournalSize > 0) && userId != account::UserId{}) {
            SaveDataAttribute attribute{};
            attribute.programId = saveDataOwnerId;
            attribute.userId = userId;
            attribute.type = SaveDataType::Account;
            const auto creationResult{CreateSaveDataDirectory(publicAppFilesPath, SaveDataSpaceId::User,
                                                              attribute, saveDataOwnerId, true)};
            if (creationResult)
                return creationResult;
        }

        if (deviceSaveDataSize > 0 || deviceJournalSize > 0) {
            SaveDataAttribute attribute{};
            attribute.programId = saveDataOwnerId;
            attribute.type = SaveDataType::Device;
            const auto creationResult{CreateSaveDataDirectory(publicAppFilesPath, SaveDataSpaceId::User,
                                                              attribute, saveDataOwnerId, true)};
            if (creationResult)
                return creationResult;
        }

        return {};
    }

    Result EnsureApplicationCacheStorage(const std::string &publicAppFilesPath, u64 saveDataOwnerId,
                                         u64 cacheStorageSize, u64 cacheStorageJournalSize) {
        // Launch-time ensure uses the legacy NACP cache size fields directly.
        // CacheStorageDataAndJournalSizeMax and CacheStorageIndexMax constrain the
        // explicit CreateCacheStorage API, not the index-zero ensure path.
        if (cacheStorageSize == 0)
            return {};

        SaveDataAttribute attribute{};
        attribute.programId = saveDataOwnerId;
        attribute.type = SaveDataType::Cache;
        attribute.rank = SaveDataRank::Primary;
        attribute.index = 0;

        const auto creationResult{CreateSaveDataDirectory(publicAppFilesPath, SaveDataSpaceId::User,
                                                          attribute, saveDataOwnerId, true)};
        if (creationResult)
            return creationResult;

        std::optional<CacheStorageMetadata> existingMetadata;
        const auto metadataResult{ReadCacheStorageMetadata(publicAppFilesPath, saveDataOwnerId, 0, existingMetadata)};
        if (metadataResult)
            return metadataResult;

        // Ensure semantics may grow an existing cache but must never shrink the
        // sizes already recorded for it.
        const u64 dataSize{existingMetadata ? std::max(existingMetadata->dataSize, cacheStorageSize) : cacheStorageSize};
        const u64 journalSize{existingMetadata ? std::max(existingMetadata->journalSize, cacheStorageJournalSize)
                                               : cacheStorageJournalSize};
        return WriteCacheStorageMetadata(publicAppFilesPath, saveDataOwnerId, 0, dataSize, journalSize);
    }

    Result CreateApplicationCacheStorage(const std::string &publicAppFilesPath, u64 saveDataOwnerId,
                                         u16 cacheStorageIndexMax, u64 cacheStorageDataAndJournalSizeMax,
                                         u64 index, i64 saveSize, i64 journalSize,
                                         CacheStorageTargetMedia &targetMedia, u64 &requiredSize) {
        targetMedia = CacheStorageTargetMedia::None;
        requiredSize = 0;
        if (saveSize < 0 || journalSize < 0)
            return result::InvalidArgument;
        if (index > cacheStorageIndexMax)
            return result::CacheStorageIndexTooLarge;

        const u64 unsignedSaveSize{static_cast<u64>(saveSize)};
        const u64 unsignedJournalSize{static_cast<u64>(journalSize)};
        if (unsignedSaveSize > std::numeric_limits<u64>::max() - unsignedJournalSize ||
            unsignedSaveSize + unsignedJournalSize > cacheStorageDataAndJournalSizeMax)
            return result::CacheStorageSizeTooLarge;
        if (index != 0)
            return result::NotImplemented;

        SaveDataAttribute attribute{};
        attribute.programId = saveDataOwnerId;
        attribute.type = SaveDataType::Cache;
        attribute.rank = SaveDataRank::Primary;
        attribute.index = static_cast<u16>(index);
        const auto creationResult{CreateSaveDataDirectory(publicAppFilesPath, SaveDataSpaceId::User,
                                                          attribute, saveDataOwnerId, false)};
        if (creationResult) {
            if (creationResult != result::AlreadyExists)
                return creationResult;

            std::optional<CacheStorageMetadata> existingMetadata;
            const auto metadataResult{ReadCacheStorageMetadata(publicAppFilesPath, saveDataOwnerId,
                                                               static_cast<u16>(index), existingMetadata)};
            if (metadataResult)
                return metadataResult;

            if (existingMetadata)
                return creationResult;

            // Old Strato builds represented cache storage only by its host
            // directory. That state cannot exist on Horizon because the save
            // metadata and directory are created as one operation. Adopt such
            // an untracked legacy directory using the explicit sizes supplied
            // by the guest, then preserve normal AlreadyExists semantics from
            // this point onward.
            const auto migrationResult{WriteCacheStorageMetadata(publicAppFilesPath, saveDataOwnerId,
                                                                 static_cast<u16>(index), unsignedSaveSize,
                                                                 unsignedJournalSize)};
            if (migrationResult)
                return migrationResult;

            targetMedia = CacheStorageTargetMedia::Nand;
            return {};
        }

        const auto metadataResult{WriteCacheStorageMetadata(publicAppFilesPath, saveDataOwnerId,
                                                            static_cast<u16>(index), unsignedSaveSize,
                                                            unsignedJournalSize)};
        if (metadataResult)
            return metadataResult;

        targetMedia = CacheStorageTargetMedia::Nand;
        return {};
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

    Result IFileSystemProxy::GetCacheStorageSize(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto index{ReadArgument<u16>(request)};
        if (!index)
            return result::InvalidArgument;
        if (*index != 0)
            return result::EntityNotFound;
        if (!state.loader || !state.loader->nacp)
            return result::EntityNotFound;

        const u64 saveDataOwnerId{state.loader->nacp->nacpContents.saveDataOwnerId};
        SaveDataAttribute attribute{};
        attribute.programId = saveDataOwnerId;
        attribute.type = SaveDataType::Cache;
        attribute.rank = SaveDataRank::Primary;
        attribute.index = *index;

        const auto saveDataPath{GetSaveDataPath(SaveDataSpaceId::User, attribute, saveDataOwnerId)};
        if (!saveDataPath)
            return result::InvalidArgument;

        try {
            vfs::OsFileSystem root{state.os->publicAppFilesPath + "/switch"};
            if (!root.DirectoryExists(*saveDataPath))
                return result::EntityNotFound;
        } catch (const std::exception &) {
            return result::UnexpectedFailure;
        }

        std::optional<CacheStorageMetadata> metadata;
        const auto metadataResult{ReadCacheStorageMetadata(state.os->publicAppFilesPath, saveDataOwnerId, *index, metadata)};
        if (metadataResult)
            return metadataResult;
        if (!metadata)
            return result::EntityNotFound;
        if (metadata->dataSize > static_cast<u64>(std::numeric_limits<i64>::max()) ||
            metadata->journalSize > static_cast<u64>(std::numeric_limits<i64>::max()))
            return result::UnexpectedFailure;

        response.Push<i64>(static_cast<i64>(metadata->dataSize));
        response.Push<i64>(static_cast<i64>(metadata->journalSize));
        return {};
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

        const std::string hostPath{state.os->publicAppFilesPath + "/switch" + *saveDataPath};
        auto [fileSystem, error]{vfs::OsFileSystem::OpenExistingWithin(hostPath, state.os->publicAppFilesPath + "/switch")};
        if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
            return result::EntityNotFound;
        if (error)
            return MapVfsError(error);

        manager.RegisterService(std::make_shared<IFileSystem>(std::move(fileSystem), state, manager, readOnly), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenSaveDataFileSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return OpenSaveDataFileSystemImpl(session, request, response, false);
    }

    Result IFileSystemProxy::OpenReadOnlySaveDataFileSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return OpenSaveDataFileSystemImpl(session, request, response, true);
    }

    Result IFileSystemProxy::OpenSaveDataInfoReader(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::vector<SaveDataInfo> entries;
        if (state.loader && state.loader->nacp) {
            std::optional<SaveDataInfo> cacheInfo;
            const u64 saveDataOwnerId{state.loader->nacp->nacpContents.saveDataOwnerId};
            const auto cacheResult{GetCacheSaveDataInfo(state.os->publicAppFilesPath,
                                                        saveDataOwnerId,
                                                        GetCurrentApplicationId(state, saveDataOwnerId),
                                                        cacheInfo)};
            if (cacheResult)
                return cacheResult;
            if (cacheInfo)
                entries.push_back(*cacheInfo);
        }

        LOGI("FSP cache diag: OpenSaveDataInfoReader entries={}", entries.size());
        manager.RegisterService(std::make_shared<ISaveDataInfoReader>(state, manager, std::move(entries)), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenSaveDataInfoReaderBySaveDataSpaceId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto spaceId{ReadArgument<SaveDataSpaceId>(request)};
        if (!spaceId || !IsValidSaveDataSpaceId(*spaceId))
            return result::InvalidArgument;

        std::vector<SaveDataInfo> entries;
        if (*spaceId == SaveDataSpaceId::User && state.loader && state.loader->nacp) {
            std::optional<SaveDataInfo> cacheInfo;
            const u64 saveDataOwnerId{state.loader->nacp->nacpContents.saveDataOwnerId};
            const auto cacheResult{GetCacheSaveDataInfo(state.os->publicAppFilesPath,
                                                        saveDataOwnerId,
                                                        GetCurrentApplicationId(state, saveDataOwnerId),
                                                        cacheInfo)};
            if (cacheResult)
                return cacheResult;
            if (cacheInfo)
                entries.push_back(*cacheInfo);
        }

        LOGI("FSP cache diag: OpenSaveDataInfoReaderBySaveDataSpaceId space={} entries={}",
             static_cast<u32>(*spaceId), entries.size());
        manager.RegisterService(std::make_shared<ISaveDataInfoReader>(state, manager, std::move(entries), *spaceId), session, response);
        return {};
    }

    Result IFileSystemProxy::OpenSaveDataInfoReaderOnlyCacheStorage(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::vector<SaveDataInfo> entries;
        if (state.loader && state.loader->nacp) {
            std::optional<SaveDataInfo> cacheInfo;
            const u64 saveDataOwnerId{state.loader->nacp->nacpContents.saveDataOwnerId};
            const auto cacheResult{GetCacheSaveDataInfo(state.os->publicAppFilesPath,
                                                        saveDataOwnerId,
                                                        GetCurrentApplicationId(state, saveDataOwnerId),
                                                        cacheInfo)};
            if (cacheResult)
                return cacheResult;
            if (cacheInfo)
                entries.push_back(*cacheInfo);
        }

        LOGI("FSP cache diag: OpenSaveDataInfoReaderOnlyCacheStorage entries={}", entries.size());
        manager.RegisterService(std::make_shared<ISaveDataInfoReader>(state, manager, std::move(entries), std::nullopt, true), session, response);
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

        if (!state.os->assetFileSystem) {
            if (archiveError)
                return result::UnexpectedFailure;
            return result::EntityNotFound;
        }
        auto assetBacking{state.os->assetFileSystem->OpenFileUnchecked(fmt::format("romfs/{:016X}", input->dataId))};
        if (!assetBacking) {
            if (archiveError)
                return result::UnexpectedFailure;
            return result::EntityNotFound;
        }
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
