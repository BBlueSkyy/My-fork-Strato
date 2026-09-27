// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstring>
#include <limits>
#include "helpers.h"
#include "validation.h"
#include "IStorage.h"

namespace skyline::service::fssrv {
    namespace {
        struct StorageRange {
            i64 offset;
            i64 size;
        };
        static_assert(sizeof(StorageRange) == 0x10);

        template<typename T>
        std::optional<T> ReadArgument(const ipc::IpcRequest &request) {
            if (!request.cmdArg || request.cmdArgSz < sizeof(T))
                return std::nullopt;
            T value{};
            std::memcpy(&value, request.cmdArg, sizeof(T));
            return value;
        }

        Result ValidateStorageRange(const StorageRange &range, size_t extent, size_t &offset, size_t &size) {
            if (range.offset < 0)
                return result::InvalidOffset;
            if (range.size < 0)
                return result::InvalidSize;
            if (!ValidateRange(range.offset, range.size, extent))
                return result::OutOfRange;
            offset = static_cast<size_t>(range.offset);
            size = static_cast<size_t>(range.size);
            return {};
        }
    }

    IStorage::IStorage(std::shared_ptr<vfs::Backing> backing, const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager), backing(std::move(backing)) {}

    Result IStorage::Read(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto range{ReadArgument<StorageRange>(request)};
        if (!range)
            return result::InvalidArgument;
        if (!backing->mode.read)
            return result::ReadNotPermitted;

        size_t offset{}, size{};
        if (const auto validation{ValidateStorageRange(*range, backing->size, offset, size)}; validation)
            return validation;
        if (size == 0)
            return {};
        if (request.outputBuf.empty() || request.outputBuf[0].size() < size)
            return result::InvalidSize;

        const auto [read, error]{backing->ReadWithError(request.outputBuf[0].first(size), offset)};
        if (error)
            return MapBackingError(error);
        return read == size ? Result{} : result::UnexpectedFailure;
    }

    Result IStorage::Write(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
        const auto range{ReadArgument<StorageRange>(request)};
        if (!range)
            return result::InvalidArgument;
        if (!backing->mode.write)
            return result::WriteNotPermitted;

        size_t offset{}, size{};
        if (const auto validation{ValidateStorageRange(*range, backing->size, offset, size)}; validation)
            return validation;
        if (size == 0)
            return {};
        if (request.inputBuf.empty() || request.inputBuf[0].size() < size)
            return result::InvalidSize;

        const auto [written, error]{backing->WriteWithError(request.inputBuf[0].first(size), offset)};
        if (error)
            return MapBackingError(error, true);
        return written == size ? Result{} : result::UnexpectedFailure;
    }

    Result IStorage::Flush(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return MapBackingError(backing->Flush(), true);
    }

    Result IStorage::SetSize(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &) {
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

    Result IStorage::GetSize(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        if (backing->size > static_cast<size_t>(std::numeric_limits<i64>::max()))
            return result::UnexpectedFailure;
        response.Push<i64>(static_cast<i64>(backing->size));
        return {};
    }
}
