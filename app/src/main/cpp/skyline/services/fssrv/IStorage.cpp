// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include "helpers.h"
#include "validation.h"
#include "IStorage.h"
#include "../../common/myhero_crash_probe.h"

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

        // Diagnostic-only. Retain the bytes the guest received in private
        // storage, then independently read the same regions from the read-only
        // RomFS. Never overwrite the guest output or alter the service result.
        // Bound both frequency and bytes: 1/16, at most 1024 checks, max 8 KiB
        // from each request. A mismatch is evidence of a storage/read issue;
        // matching re-reads do not prove the original content is authentic.
        void RomfsIndependentRecheck(vfs::Backing &backing, std::size_t offset,
                                     span<u8> guestOutput) {
            using namespace diagnostics::myhero;
            const auto n{romfsReadCount.load(std::memory_order_relaxed)};
            if (n % 16 != 0)
                return;
            const auto count{romfsRecheckCount.fetch_add(1, std::memory_order_relaxed) + 1};
            if (count > 1024)
                return;

            constexpr std::size_t SegmentMax{4096};
            std::array<u8, SegmentMax> guestCopy{};
            std::array<u8, SegmentMax> repeat{};
            bool matched{true};
            std::size_t badAt{};
            int readError{};
            for (int part{}; part < 2; ++part) {
                const auto length{std::min<std::size_t>(guestOutput.size(), SegmentMax)};
                const auto start{part == 0 ? 0 : guestOutput.size() - length};
                if (part == 1 && start == 0)
                    break;
                // Snapshot the guest output before rereading the backing.
                std::memcpy(guestCopy.data(), guestOutput.data() + start, length);
                const auto [read, error]{backing.ReadWithError(
                    span<u8>(repeat.data(), length), offset + start)};
                readError = error.value();
                if (readError || read != length ||
                    std::memcmp(guestCopy.data(), repeat.data(), length) != 0) {
                    matched = false;
                    badAt = start;
                    break;
                }
            }
            if (!matched) {
                romfsRecheckFailureCount.fetch_add(1, std::memory_order_relaxed);
                Line line;
                line.Add("ROMFS_RECHECK_MISMATCH ms=");
                line.Decimal(MonotonicMs());
                line.Add(" check=");
                line.Decimal(count);
                line.Add(" offset=");
                line.Hex(offset + badAt);
                line.Add(" req=");
                line.Decimal(guestOutput.size());
                line.Add(" err=");
                line.Decimal(static_cast<std::uint64_t>(readError));
                line.Write();
            }
            if (count % 128 == 0 || !matched) {
                Line summary;
                summary.Add("ROMFS_RECHECK_STATS checked=");
                summary.Decimal(count);
                summary.Add(" mismatches=");
                summary.Decimal(romfsRecheckFailureCount.load(std::memory_order_relaxed));
                summary.Write();
            }
        }
    }

    IStorage::IStorage(std::shared_ptr<vfs::Backing> backing, const DeviceState &state,
                       ServiceManager &manager, bool currentProcessRomfs)
        : BaseService(state, manager), backing(std::move(backing)),
          traceCurrentRomfs(currentProcessRomfs && diagnostics::myhero::Enabled()) {
        if (traceCurrentRomfs) {
            diagnostics::myhero::Line line;
            line.Add("ROMFS_OPEN size=");
            line.Decimal(this->backing->size);
            line.Write();
        }
    }

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
        if (traceCurrentRomfs) {
            diagnostics::myhero::RomfsRead(offset, size, read, error.value(),
                                           request.outputBuf[0].data(), read);
            if (!error && read == size)
                RomfsIndependentRecheck(*backing, offset, request.outputBuf[0].first(size));
        }
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
