// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cstring>
#include <limits>

#include "ISslContext.h"
#include "ISslService.h"
#include "tls_backend.h"

namespace skyline::service::ssl {
    namespace {
        template<typename T>
        std::optional<T> GetArgument(const ipc::IpcRequest &request, size_t offset = 0) {
            if (!request.cmdArg || offset > request.cmdArgSz || request.cmdArgSz - offset < sizeof(T))
                return std::nullopt;

            T value{};
            std::memcpy(&value, request.cmdArg + offset, sizeof(value));
            return value;
        }

        std::optional<span<u8>> GetInputBuffer(ipc::IpcRequest &request, size_t index = 0) {
            if (request.inputBuf.size() <= index || !request.inputBuf[index].data() || request.inputBuf[index].empty())
                return std::nullopt;
            return request.inputBuf[index];
        }

        std::optional<span<u8>> GetOutputBuffer(ipc::IpcRequest &request, size_t index = 0) {
            if (request.outputBuf.size() <= index || !request.outputBuf[index].data())
                return std::nullopt;
            return request.outputBuf[index];
        }

        ResultValue<std::vector<i32>> GetCertificateIds(ipc::IpcRequest &request) {
            auto input{GetInputBuffer(request)};
            if (!input || input->size() % sizeof(i32))
                return result::InvalidCertificateBufferSize;

            std::vector<i32> ids(input->size() / sizeof(i32));
            std::memcpy(ids.data(), input->data(), input->size());
            return ids;
        }

        ResultValue<size_t> GetCertificatesSize(const std::vector<const CertificateStoreCertificate *> &certificates, bool includeTerminator) {
            constexpr size_t Alignment{4};
            if (certificates.size() > std::numeric_limits<size_t>::max() / sizeof(BuiltInCertificateInfo) - static_cast<size_t>(includeTerminator))
                return result::InsufficientMemory;

            size_t size{(certificates.size() + static_cast<size_t>(includeTerminator)) * sizeof(BuiltInCertificateInfo)};
            for (const auto *certificate : certificates) {
                const size_t padding{(Alignment - (size % Alignment)) % Alignment};
                if (padding > std::numeric_limits<size_t>::max() - size ||
                    certificate->der.size() > std::numeric_limits<size_t>::max() - size - padding)
                    return result::InsufficientMemory;
                size += padding + certificate->der.size();
            }
            return size;
        }

        ResultValue<std::string> GetHostname(ipc::IpcRequest &request) {
            auto input{GetInputBuffer(request)};
            if (!input || input->size() > MaximumHostnameLength + 1)
                return result::InvalidOption;

            auto end{std::find(input->begin(), input->end(), 0)};
            if (end == input->end())
                return result::InvalidOption;
            const size_t length{static_cast<size_t>(end - input->begin())};
            if (!length || length > MaximumHostnameLength)
                return result::InvalidOption;
            if (end != input->end() && std::any_of(end + 1, input->end(), [](u8 byte) { return byte != 0; }))
                return result::InvalidOption;
            return std::string(reinterpret_cast<const char *>(input->data()), length);
        }
    }

    ISslService::ISslService(const DeviceState &state, ServiceManager &manager, std::shared_ptr<SslSharedState> sharedState,
                             ServicePermission permission)
        : BaseService(state, manager), sharedState(std::move(sharedState)), permission(permission) {}

    void ISslService::OnSessionClosed(const type::KSession &session) {
        std::scoped_lock lock{sessionStateMutex};
        sessionStates.erase(&session);
    }

    ISslService::SessionState ISslService::GetSessionState(const type::KSession &session) {
        std::scoped_lock lock{sessionStateMutex};
        return sessionStates[&session];
    }

    Result ISslService::CreateContextImpl(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response, bool systemContext) {
        if (systemContext && permission != ServicePermission::System)
            return result::AccessDenied;

        auto rawVersion{GetArgument<u32>(request)};
        if (!rawVersion || request.cmdArgSz < 0x10)
            return result::InvalidOption;

        SslVersion version{*rawVersion};
        if (Result rc{ValidateSslVersion(version)}; rc)
            return rc;

        const SessionState sessionState{GetSessionState(session)};
        manager.RegisterService(std::make_shared<ISslContext>(state, manager, sharedState, version, sessionState.interfaceVersion,
                                                              permission, sessionState.allowDisableVerifyOption),
                                session, response);
        return {};
    }

    Result ISslService::CreateContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return CreateContextImpl(session, request, response, false);
    }

    Result ISslService::CreateContextForSystem(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return CreateContextImpl(session, request, response, true);
    }

    Result ISslService::GetContextCount(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &response) {
        response.Push<u32>(sharedState->contextCount.load());
        return {};
    }

    Result ISslService::GetCertificateBufSize(type::KSession &, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto ids{GetCertificateIds(request)};
        if (!ids)
            return ids.result;
        auto selected{sharedState->certificateStore.Select(*ids)};
        if (!selected)
            return selected.result;
        const bool includeTerminator{ids->size() == 1 && ids->front() == -1};
        auto size{GetCertificatesSize(*selected, includeTerminator)};
        if (!size)
            return size.result;
        if (*size > std::numeric_limits<u32>::max())
            return result::CertificateBufferTooSmall;
        response.Push<u32>(static_cast<u32>(*size));
        return {};
    }

    Result ISslService::GetCertificates(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        auto ids{GetCertificateIds(request)};
        auto output{GetOutputBuffer(request)};
        if (!ids || !output)
            return result::InvalidCertificateBufferSize;
        auto selected{sharedState->certificateStore.Select(*ids)};
        if (!selected)
            return selected.result;
        const bool includeTerminator{ids->size() == 1 && ids->front() == -1};
        auto requiredSize{GetCertificatesSize(*selected, includeTerminator)};
        if (!requiredSize)
            return requiredSize.result;
        if (output->size() < *requiredSize)
            return result::CertificateBufferTooSmall;

        std::fill(output->begin(), output->end(), 0);
        const size_t infoSize{(selected->size() + static_cast<size_t>(includeTerminator)) * sizeof(BuiltInCertificateInfo)};
        size_t dataOffset{infoSize};
        for (size_t index{}; index < selected->size(); ++index) {
            const auto &certificate{*(*selected)[index]};
            dataOffset = (dataOffset + 3) & ~size_t{3};
            BuiltInCertificateInfo info{certificate.id, certificate.status, certificate.der.size(), dataOffset};
            std::memcpy(output->data() + index * sizeof(info), &info, sizeof(info));
            std::memcpy(output->data() + dataOffset, certificate.der.data(), certificate.der.size());
            dataOffset += certificate.der.size();
        }
        if (includeTerminator) {
            const BuiltInCertificateInfo terminator{-1, -1, 0, 0};
            std::memcpy(output->data() + selected->size() * sizeof(terminator), &terminator, sizeof(terminator));
        }

        if (GetSessionState(session).interfaceVersion >= 1)
            response.Push<u32>(static_cast<u32>(selected->size()));
        return {};
    }

    Result ISslService::DebugIoctl(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return result::InternalLogicError;
    }

    Result ISslService::SetInterfaceVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        auto version{GetArgument<u32>(request)};
        if (!version || *version < 1 || *version > 5)
            return result::InvalidOption;

        std::scoped_lock lock{sessionStateMutex};
        sessionStates[&session].interfaceVersion = *version;
        return {};
    }

    Result ISslService::FlushSessionCache(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        if (GetSessionState(session).interfaceVersion < 2)
            return result::InvalidOption;
        auto rawOption{GetArgument<u32>(request)};
        if (!rawOption)
            return result::InvalidOption;

        u32 count{};
        switch (static_cast<FlushSessionCacheOption>(*rawOption)) {
            case FlushSessionCacheOption::SingleHost: {
                auto hostname{GetHostname(request)};
                if (!hostname)
                    return hostname.result;
                count = sharedState->sessionCache->Flush(*hostname);
                break;
            }
            case FlushSessionCacheOption::AllHosts:
                if (!request.inputBuf.empty() && !request.inputBuf.front().empty())
                    return result::InvalidOption;
                count = sharedState->sessionCache->Flush(std::nullopt);
                break;
            default:
                return result::InvalidOption;
        }
        response.Push<u32>(count);
        return {};
    }

    Result ISslService::SetDebugOption(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (GetSessionState(session).interfaceVersion < 3)
            return result::InvalidOption;
        auto rawOption{GetArgument<u32>(request)};
        auto input{GetInputBuffer(request)};
        if (!rawOption || !input || static_cast<DebugOption>(*rawOption) != DebugOption::AllowDisableVerifyOption)
            return result::InvalidOption;

        std::scoped_lock lock{sessionStateMutex};
        sessionStates[&session].allowDisableVerifyOption = input->front() != 0;
        return {};
    }

    Result ISslService::GetDebugOption(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &) {
        if (GetSessionState(session).interfaceVersion < 3)
            return result::InvalidOption;
        auto rawOption{GetArgument<u32>(request)};
        auto output{GetOutputBuffer(request)};
        if (!rawOption || !output || output->empty() || static_cast<DebugOption>(*rawOption) != DebugOption::AllowDisableVerifyOption)
            return result::InvalidOption;
        output->front() = GetSessionState(session).allowDisableVerifyOption;
        return {};
    }

    Result ISslService::ClearTls12FallbackFlag(type::KSession &session, ipc::IpcRequest &, ipc::IpcResponse &) {
        if (GetSessionState(session).interfaceVersion < 3)
            return result::InvalidOption;
        std::scoped_lock lock{sessionStateMutex};
        sessionStates[&session].tls12FallbackCleared = true;
        return {};
    }

    Result ISslService::UnsupportedModernCommand(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return result::InternalLogicError;
    }

    Result ISslService::SetThreadCoreMask(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return permission == ServicePermission::System ? result::InternalLogicError : result::AccessDenied;
    }

    Result ISslService::GetThreadCoreMask(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return permission == ServicePermission::System ? result::InternalLogicError : result::AccessDenied;
    }

    Result ISslService::VerifySignature(type::KSession &, ipc::IpcRequest &, ipc::IpcResponse &) {
        return permission == ServicePermission::System ? result::InternalLogicError : result::AccessDenied;
    }
}
