// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstring>
#include <limits>
#include "helpers.h"
#include "validation.h"
#include <kernel/types/KProcess.h>
#include <kernel/types/KThread.h>
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
        if (!input) {
            // A malformed IFile::Write request and an invalid WriteOption
            // otherwise collapse into the same result (2-6001).
            LOGW("FSP_WRITE_DIAG: Write missing 0x18-byte arguments: raw_args=0x{:X}, ipc_raw_words={}, tipc={}, domain={}, input_buffers={}",
                 request.cmdArgSz, static_cast<u32>(request.header->rawSize),
                 request.isTipc, request.isDomain, request.inputBuf.size());
            return result::InvalidArgument;
        }
        if (input->option & ~1U) {
            // Diagnostic only: preserve the SDK's invalid-option error.
            // Report the decoded 24-byte payload and transport shape so we
            // can distinguish guest-supplied flags from IPC offset corruption.
            LOGW("FSP_WRITE_DIAG: Write rejected invalid option=0x{:08X}, reserved=0x{:08X}, offset={}, size={}, raw_args=0x{:X}, ipc_raw_words={}, tipc={}, domain={}, input_buffers={}, input0_bytes=0x{:X}, open_mode=0x{:X}, file_size=0x{:X}",
                 input->option, input->padding, input->offset, input->size,
                 request.cmdArgSz, static_cast<u32>(request.header->rawSize),
                 request.isTipc, request.isDomain, request.inputBuf.size(),
                 request.inputBuf.empty() ? size_t{} : request.inputBuf[0].size(),
                 backing->mode.raw, backing->size);
            // Inspect only IPC framing, not file contents or file names.
            // Distinguish an incorrectly positioned CMIF argument pointer
            // from a bad WriteOption already present in the guest's request.
            if (request.isDomain && request.domain && request.payload && request.cmdArg) {
                const auto tlsBegin{reinterpret_cast<uintptr_t>(state.thread->tlsRegion)};
                const auto argAddress{reinterpret_cast<uintptr_t>(request.cmdArg)};
                const auto payloadAddress{reinterpret_cast<uintptr_t>(request.payload)};
                const auto domainAddress{reinterpret_cast<uintptr_t>(request.domain)};
                if (argAddress >= tlsBegin && argAddress - tlsBegin <= constant::TlsIpcSize &&
                    payloadAddress >= tlsBegin && payloadAddress - tlsBegin <= constant::TlsIpcSize - sizeof(ipc::PayloadHeader) &&
                    domainAddress >= tlsBegin && domainAddress - tlsBegin <= constant::TlsIpcSize - sizeof(ipc::DomainHeaderRequest)) {
                    LOGW("FSP_WRITE_DIAG: Domain IPC framing: arg_tls=0x{:X}, cmif_tls=0x{:X}, domain_tls=0x{:X}, domain_payload_size=0x{:X}, domain_cmd={}, domain_object=0x{:X}, cmif_magic=0x{:08X}, cmif_version=0x{:X}, cmif_cmd=0x{:X}, cmif_token=0x{:X}",
                         argAddress - tlsBegin, payloadAddress - tlsBegin, domainAddress - tlsBegin,
                         request.domain->payloadSz, static_cast<u32>(request.domain->command),
                         request.domain->objectId, static_cast<u32>(request.payload->magic),
                         request.payload->version, request.payload->value, request.payload->token);
                } else {
                    LOGW("FSP_WRITE_DIAG: Domain IPC framing pointers outside TLS command buffer");
                }
            }
            // The previous stack walker stopped the log before IFile::Write
            // could return its original InvalidArgument. Do not read or
            // symbolize guest stack memory synchronously inside IPC dispatch:
            // rely only on already-captured NCE CPU state for this diagnostic.
            if (state.process->is64bit()) {
                const auto &guest{static_cast<const type::KNceThread &>(*state.thread).ctx};
                const auto &call{guest.svcCallsite};
                LOGW("FSP_WRITE_DIAG: Write caller: PC=0x{:X}, LR=0x{:X}, SP=0x{:X}, FP=0x{:X}",
                     call.pc, call.lr, call.sp, call.fp);
                LOGW("FSP_WRITE_DIAG: Saved SVC registers: X0=0x{:X}, X1=0x{:X}, X2=0x{:X}, X3=0x{:X}, X8=0x{:X}, X16=0x{:X}, X17=0x{:X}, X18=0x{:X}",
                     guest.gpr.x0, guest.gpr.x1, guest.gpr.x2, guest.gpr.x3,
                     guest.gpr.x8, guest.gpr.x16, guest.gpr.x17, guest.gpr.x18);
            }
            LOGW("FSP_WRITE_DIAG: returning original InvalidArgument (2-6001) without guest stack traversal");
            return result::InvalidArgument;
        }
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
