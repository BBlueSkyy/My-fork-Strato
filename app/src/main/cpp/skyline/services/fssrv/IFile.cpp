// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstring>
#include <limits>
#include "helpers.h"
#include "validation.h"
#include "IFile.h"

namespace skyline::service::fssrv {
    namespace {
        struct FileIoInput {
            u32 option;
            u32 padding;
            i64 offset;
            i64 size;
        };
        static_assert(sizeof(FileIoInput) == 0x18);

        template<typename T>
        std::optional<T> ReadArgument(const ipc::IpcRequest &request) {
            if (!request.cmdArg || request.cmdArgSz < sizeof(T))
                return std::nullopt;
            T value{};
            std::memcpy(&value, request.cmdArg, sizeof(T));
            return value;
        }

        Result ValidateFileIo(const FileIoInput &input, size_t &offset, size_t &size) {
            if (input.offset < 0)
                return result::InvalidOffset;
            if (input.size < 0)
                return result::InvalidSize;
            const auto checkedOffset{ToSize(input.offset)};
            const auto checkedSize{ToSize(input.size)};
            if (!checkedOffset || !checkedSize || *checkedSize > std::numeric_limits<size_t>::max() - *checkedOffset)
                return result::OutOfRange;
            offset = *checkedOffset;
            size = *checkedSize;
            return {};
        }
    }

    IFile::IFile(std::shared_ptr<vfs::Backing> backing, const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager), backing(std::move(backing)) {}

    Result IFile::Read(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto input{ReadArgument<FileIoInput>(request)};
        if (!input)
            return result::InvalidArgument;
        if (input->option != 0)
            return result::InvalidArgument;
        if (!backing->mode.read)
            return result::ReadNotPermitted;

        size_t offset{}, requestedSize{};
        if (const auto validation{ValidateFileIo(*input, offset, requestedSize)}; validation)
            return validation;
        if (offset > backing->size)
            return result::OutOfRange;
        if (requestedSize != 0 && (request.outputBuf.empty() || request.outputBuf[0].size() < requestedSize))
            return result::InvalidSize;

        const auto readSize{std::min(requestedSize, backing->size - offset)};
        if (readSize != 0) {
            const auto [read, error]{backing->ReadWithError(request.outputBuf[0].first(readSize), offset)};
            if (error)
                return MapBackingError(error);
            if (read != readSize)
                return result::UnexpectedFailure;
        }
        response.Push<u64>(readSize);
        return {};
    }

    Result IFile::Write(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto input{ReadArgument<FileIoInput>(request)};
        if (!input)
            return result::InvalidArgument;
        if (input->option & ~1U)
            return result::InvalidArgument;
        if (!backing->mode.write)
            return result::WriteNotPermitted;

        size_t offset{}, size{};
        if (const auto validation{ValidateFileIo(*input, offset, size)}; validation)
            return validation;
        if (size != 0 && (request.inputBuf.empty() || request.inputBuf[0].size() < size))
            return result::InvalidSize;

        const auto end{offset + size};
        if (end > backing->size) {
            if (!backing->mode.append)
                return result::FileExtensionWithoutOpenModeAllowAppend;
            if (const auto mapped{MapBackingError(backing->ResizeWithError(end), true)}; mapped)
                return mapped;
        }

        if (size != 0) {
            const auto [written, error]{backing->WriteWithError(request.inputBuf[0].first(size), offset)};
            if (error)
                return MapBackingError(error, true);
            if (written != size)
                return result::UnexpectedFailure;
        }
        return (input->option & 1U) ? MapBackingError(backing->Flush(), true) : Result{};
    }

    Result IFile::Flush(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return MapBackingError(backing->Flush(), true);
    }

    Result IFile::SetSize(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto rawSize{ReadArgument<i64>(request)};
        if (!rawSize)
            return result::InvalidArgument;
        if (*rawSize < 0)
            return result::InvalidSize;
        if (!backing->mode.write)
            return result::WriteNotPermitted;
        const auto size{ToSize(*rawSize)};
        if (!size)
            return result::OutOfRange;
        return MapBackingError(backing->ResizeWithError(*size), true);
    }

    Result IFile::GetSize(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (backing->size > static_cast<size_t>(std::numeric_limits<i64>::max()))
            return result::UnexpectedFailure;
        response.Push<i64>(static_cast<i64>(backing->size));
        return {};
    }
}
