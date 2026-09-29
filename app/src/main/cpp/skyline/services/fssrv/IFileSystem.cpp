// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstring>
#include <limits>
#include "helpers.h"
#include "validation.h"
#include "IFile.h"
#include "IDirectory.h"
#include "IFileSystem.h"

namespace skyline::service::fssrv {
    namespace {
        struct CreateFileInput {
            u32 option;
            u32 padding;
            i64 size;
        };
        static_assert(sizeof(CreateFileInput) == 0x10);

        template<typename T>
        std::optional<T> ReadArgument(const ipc::IpcRequest &request, size_t offset = 0) {
            if (!request.cmdArg || offset > request.cmdArgSz || request.cmdArgSz - offset < sizeof(T))
                return std::nullopt;
            T value{};
            std::memcpy(&value, request.cmdArg + offset, sizeof(T));
            return value;
        }

        std::optional<std::string> RequestPath(ipc::IpcRequest &request, size_t index = 0) {
            if (index >= request.inputBuf.size())
                return std::nullopt;
            return ReadPath(request.inputBuf[index]);
        }
    }

    IFileSystem::IFileSystem(std::shared_ptr<vfs::FileSystem> backing, const DeviceState &state, ServiceManager &manager, bool readOnly)
        : BaseService(state, manager), backing(std::move(backing)), readOnly(readOnly || this->backing->IsReadOnly()) {}

    Result IFileSystem::CreateFile(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto path{RequestPath(request)};
        const auto input{ReadArgument<CreateFileInput>(request)};
        if (!path)
            return result::InvalidPath;
        if (!input)
            return result::InvalidArgument;
        if (input->option & ~1U)
            return result::InvalidArgument;
        if (input->option & 1U)
            return result::NotImplemented;
        const auto size{ToSize(input->size)};
        if (!size)
            return result::InvalidSize;
        return MapVfsError(backing->CreateFile(*path, *size));
    }

    Result IFileSystem::CreateDirectory(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        return MapVfsError(backing->CreateDirectory(*path, false));
    }

    Result IFileSystem::GetEntryType(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        const auto type{backing->GetEntryType(*path)};
        if (!type)
            return result::PathDoesNotExist;
        response.Push(static_cast<u32>(*type));
        return {};
    }

    Result IFileSystem::OpenFile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto path{RequestPath(request)};
        const auto mode{ReadArgument<vfs::Backing::Mode>(request)};
        if (!path)
            return result::InvalidPath;
        if (!mode || !IsOpenModeValid(*mode))
            return result::InvalidOpenMode;
        if (!IsMutationAllowed(readOnly, *mode))
            return result::WriteNotPermitted;

        const auto type{backing->GetEntryType(*path)};
        if (!type || *type != vfs::Directory::EntryType::File)
            return result::PathDoesNotExist;
        auto [file, error]{backing->OpenFileWithError(*path, *mode)};
        if (error)
            return MapVfsError(error);
        if (!file)
            return result::UnexpectedFailure;
        manager.RegisterService(std::make_shared<IFile>(std::move(file), state, manager), session, response);
        return {};
    }

    Result IFileSystem::DeleteFile(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        return MapVfsError(backing->DeleteFile(*path));
    }

    Result IFileSystem::DeleteDirectory(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        return MapVfsError(backing->DeleteDirectory(*path));
    }

    Result IFileSystem::DeleteDirectoryRecursively(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        return MapVfsError(backing->DeleteDirectoryRecursively(*path));
    }

    Result IFileSystem::RenameFile(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto oldPath{RequestPath(request)};
        const auto newPath{RequestPath(request, 1)};
        if (!oldPath || !newPath)
            return result::InvalidPath;
        return MapVfsError(backing->RenameFile(*oldPath, *newPath));
    }

    Result IFileSystem::RenameDirectory(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto oldPath{RequestPath(request)};
        const auto newPath{RequestPath(request, 1)};
        if (!oldPath || !newPath)
            return result::InvalidPath;
        return MapVfsError(backing->RenameDirectory(*oldPath, *newPath));
    }

    Result IFileSystem::OpenDirectory(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto path{RequestPath(request)};
        const auto rawMode{ReadArgument<u32>(request)};
        if (!path)
            return result::InvalidPath;
        if (!rawMode || !IsDirectoryModeValid(*rawMode))
            return result::InvalidOpenMode;

        vfs::Directory::ListMode mode{};
        mode.raw = *rawMode;
        auto directory{backing->OpenDirectoryUnchecked(*path, mode)};
        if (!directory)
            return result::PathDoesNotExist;
        manager.RegisterService(std::make_shared<IDirectory>(std::move(directory), backing, state, manager), session, response);
        return {};
    }

    Result IFileSystem::CommitBacking() {
        return MapVfsError(backing->Commit());
    }

    Result IFileSystem::Commit(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return CommitBacking();
    }

    Result IFileSystem::GetFreeSpaceSize(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        u64 free{}, total{};
        if (const auto mapped{MapVfsError(backing->GetSpace(*path, free, total))}; mapped)
            return mapped;
        if (free > static_cast<u64>(std::numeric_limits<i64>::max()))
            return result::UnexpectedFailure;
        response.Push<i64>(static_cast<i64>(free));
        return {};
    }

    Result IFileSystem::GetTotalSpaceSize(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        u64 free{}, total{};
        if (const auto mapped{MapVfsError(backing->GetSpace(*path, free, total))}; mapped)
            return mapped;
        if (total > static_cast<u64>(std::numeric_limits<i64>::max()))
            return result::UnexpectedFailure;
        response.Push<i64>(static_cast<i64>(total));
        return {};
    }

    Result IFileSystem::CleanDirectoryRecursively(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (readOnly)
            return result::WriteNotPermitted;
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        return MapVfsError(backing->CleanDirectoryRecursively(*path));
    }

    Result IFileSystem::GetFileTimeStampRaw(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto path{RequestPath(request)};
        if (!path)
            return result::InvalidPath;
        vfs::FileTimeStamp timestamp{};
        if (const auto mapped{MapVfsError(backing->GetFileTimeStamp(*path, timestamp))}; mapped)
            return mapped;
        response.Push(FileTimeStampRaw{
            .created = timestamp.created,
            .modified = timestamp.modified,
            .accessed = timestamp.accessed,
            .isValid = static_cast<u8>(timestamp.isValid),
        });
        return {};
    }

    Result IFileSystem::GetFileSystemAttribute(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        vfs::FileSystemAttribute source{};
        if (const auto mapped{MapVfsError(backing->GetFileSystemAttribute(source))}; mapped)
            return mapped;

        FileSystemAttribute attribute{};
        const auto pathLimit = [](const std::optional<i32> &value) -> std::optional<i32> {
            return value ? std::optional{std::min(*value, static_cast<i32>(FspPathSize - 1))} : std::nullopt;
        };
        const auto directoryPathLimit{pathLimit(source.directoryPathLengthMax)};
        const auto filePathLimit{pathLimit(source.filePathLengthMax)};

        if (source.directoryNameLengthMax) {
            attribute.directoryNameLengthMaxHasValue = true;
            attribute.directoryNameLengthMax = *source.directoryNameLengthMax;
        }
        if (source.fileNameLengthMax) {
            attribute.fileNameLengthMaxHasValue = true;
            attribute.fileNameLengthMax = *source.fileNameLengthMax;
        }
        if (directoryPathLimit) {
            attribute.directoryPathLengthMaxHasValue = true;
            attribute.directoryPathLengthMax = *directoryPathLimit;
        }
        if (filePathLimit) {
            attribute.filePathLengthMaxHasValue = true;
            attribute.filePathLengthMax = *filePathLimit;
        }

        response.Push(attribute);
        return {};
    }
}
