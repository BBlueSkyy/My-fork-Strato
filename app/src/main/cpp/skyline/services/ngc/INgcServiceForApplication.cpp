// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "INgcServiceForApplication.h"
#include <limits>
#include <kernel/results.h>
#include "content_reader.h"

namespace skyline::service::ngc {
    namespace {
        struct Arguments {
            u32 regionMask;
            dictionary::Options option;
        };
        static_assert(sizeof(Arguments) == 0x18);
        static_assert(sizeof(dictionary::Options) == 0x14);

        Result Translate(dictionary::Error error) {
            switch (error) {
                case dictionary::Error::Success: return {};
                case dictionary::Error::Unavailable:
                case dictionary::Error::Malformed: return {146, 102}; // NGC DataAccessError
                case dictionary::Error::InvalidUtf8: return {146, 5};
                case dictionary::Error::Unsupported: return {10, 1}; // SF NotSupported
            }
            return {10, 1};
        }

        std::optional<Arguments> ReadArguments(const ipc::IpcRequest &request) {
            if (!request.header || request.isTipc || !request.cmdArg || request.cmdArgSz < sizeof(Arguments))
                return std::nullopt;
            // IpcRequest::cmdArgSz overstates non-domain CMIF arguments: its
            // rawSize still includes the payload header and alignment padding.
            // Domain payloadSz can also underflow in the generic parser. Bound
            // this service's reads by the actual 0x100-byte TLS wire frame.
            constexpr size_t TlsSize{0x100};
            constexpr size_t Padding{0x10};
            const auto base{reinterpret_cast<std::uintptr_t>(request.header)};
            if (base > std::numeric_limits<std::uintptr_t>::max() - TlsSize)
                return std::nullopt;
            ipc::CommandHeader header;
            std::memcpy(&header, request.header, sizeof(header));
            size_t rawStart{sizeof(header)};
            if (header.handleDesc) {
                if (reinterpret_cast<std::uintptr_t>(request.handleDesc) != base + rawStart)
                    return std::nullopt;
                ipc::HandleDescriptor handles;
                std::memcpy(&handles, reinterpret_cast<const void *>(base + rawStart), sizeof(handles));
                rawStart += sizeof(handles) + (handles.sendPid ? sizeof(u64) : 0) +
                            (handles.copyCount + handles.moveCount) * sizeof(u32);
            }
            rawStart += header.xNo * sizeof(ipc::BufferDescriptorX) +
                        (header.aNo + header.bNo + header.wNo) * sizeof(ipc::BufferDescriptorABW);
            const size_t rawBytes{header.rawSize * sizeof(u32)};
            if (rawStart > TlsSize || rawBytes > TlsSize - rawStart)
                return std::nullopt;
            const size_t frontPadding{(Padding - rawStart % Padding) % Padding};
            const size_t backPadding{Padding - frontPadding};
            if (rawBytes < frontPadding + backPadding)
                return std::nullopt;
            const size_t contentEnd{rawStart + rawBytes - backPadding};
            size_t payloadStart{rawStart + frontPadding};
            const bool domain{request.isDomain && (header.type == ipc::CommandType::Request || header.type == ipc::CommandType::RequestWithContext)};
            std::optional<size_t> domainSize;
            if (domain) {
                if (sizeof(ipc::DomainHeaderRequest) > contentEnd - payloadStart ||
                    reinterpret_cast<std::uintptr_t>(request.domain) != base + payloadStart)
                    return std::nullopt;
                ipc::DomainHeaderRequest domainHeader;
                std::memcpy(&domainHeader, request.domain, sizeof(domainHeader));
                payloadStart += sizeof(domainHeader);
                if (domainHeader.payloadSz < sizeof(ipc::PayloadHeader) || domainHeader.payloadSz > contentEnd - payloadStart ||
                    domainHeader.inputCount * sizeof(u32) > contentEnd - payloadStart - domainHeader.payloadSz)
                    return std::nullopt;
                domainSize = domainHeader.payloadSz - sizeof(ipc::PayloadHeader);
            }
            if (sizeof(ipc::PayloadHeader) > contentEnd - payloadStart ||
                reinterpret_cast<std::uintptr_t>(request.payload) != base + payloadStart)
                return std::nullopt;
            const size_t argsStart{payloadStart + sizeof(ipc::PayloadHeader)};
            const size_t argsSize{domainSize.value_or(contentEnd - argsStart)};
            if (argsSize < sizeof(Arguments) || reinterpret_cast<std::uintptr_t>(request.cmdArg) != base + argsStart)
                return std::nullopt;
            Arguments args;
            std::memcpy(&args, request.cmdArg, sizeof(args));
            return args;
        }

        template<class Buffers> bool HasBuffer(const Buffers &buffers) {
            return !buffers.empty() && (buffers[0].empty() || buffers[0].data());
        }

        bool HasMapAliasShape(const ipc::IpcRequest &request, bool mask) {
            if (!request.header)
                return false;
            ipc::CommandHeader header;
            std::memcpy(&header, request.header, sizeof(header));
            // The generic parser merges X/A inputs and B/W/C outputs. NGC's
            // ABI specifically requires A (and B for Mask), so accepting the
            // first merged vector alone would accept the wrong transport.
            // Reject extra descriptors rather than guessing their association.
            return header.aNo == 1 && header.bNo == (mask ? 1 : 0) && !header.xNo && !header.wNo &&
                   header.cFlag == ipc::BufferCFlag::None && request.inputBuf.size() == 1 &&
                   request.outputBuf.size() == (mask ? 1U : 0U);
        }
    }

    INgcServiceForApplication::INgcServiceForApplication(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager), contentReader{std::make_shared<ContentReader>(state)},
          readFile{[reader = contentReader](std::string_view name) -> std::optional<std::vector<u8>> {
              std::vector<u8> data;
              if (!reader->ReadFile(name, data)) return std::nullopt;
              return data;
          }}, reloadContent{[reader = contentReader] { return reader->Reload(); }}, filter{readFile} {}

    INgcServiceForApplication::INgcServiceForApplication(const DeviceState &state, ServiceManager &manager,
                                                       dictionary::FileReader reader, std::function<bool()> reload)
        : BaseService(state, manager), readFile{std::move(reader)}, reloadContent{std::move(reload)}, filter{readFile} {}

    Result INgcServiceForApplication::GetContentVersion(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        std::scoped_lock lock{mutex};
        u32 version{};
        // The reference treats unreadable version data as version zero. This
        // does not imply that Check/Mask can run without dictionaries.
        const auto data{readFile("version.dat")};
        if (data && data->size() >= sizeof(version))
            version = (static_cast<u32>((*data)[0]) << 24) | (static_cast<u32>((*data)[1]) << 16) |
                      (static_cast<u32>((*data)[2]) << 8) | (*data)[3];
        response.Push(version);
        return {};
    }

    Result INgcServiceForApplication::Check(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto args{ReadArguments(request)};
        if (!args || !HasMapAliasShape(request, false) || !HasBuffer(request.inputBuf))
            return kernel::result::InvalidArgument;
        const auto input{request.inputBuf[0]};
        const std::string_view text{input.empty() ? "" : reinterpret_cast<const char *>(input.data()), input.size()};
        std::scoped_lock lock{mutex};
        const auto result{filter.Check(text, args->regionMask, args->option)};
        if (result.error != dictionary::Error::Success)
            return Translate(result.error);
        response.Push(result.value);
        return {};
    }

    Result INgcServiceForApplication::Mask(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto args{ReadArguments(request)};
        if (!args || !HasMapAliasShape(request, true) || !HasBuffer(request.inputBuf) || !HasBuffer(request.outputBuf))
            return kernel::result::InvalidArgument;
        const auto input{request.inputBuf[0]};
        const auto output{request.outputBuf[0]};
        const size_t length{std::min(input.size(), output.size())};
        std::scoped_lock lock{mutex};
        // Service.cs copies only this prefix, then filters the complete output
        // buffer. Keep that contract for shorter and larger output descriptors.
        if (length)
            std::memmove(output.data(), input.data(), length);
        std::string text{output.empty() ? "" : reinterpret_cast<const char *>(output.data()), output.size()};
        const auto result{filter.Mask(text, args->regionMask, args->option)};
        if (result.error != dictionary::Error::Success)
            return Translate(result.error);
        if (!text.empty())
            std::memcpy(output.data(), text.data(), std::min(text.size(), output.size()));
        response.Push(static_cast<i32>(result.value));
        return {};
    }

    Result INgcServiceForApplication::Reload(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        std::scoped_lock lock{mutex};
        if (reloadContent && !reloadContent())
            return {146, 102};
        return {};
    }
}
