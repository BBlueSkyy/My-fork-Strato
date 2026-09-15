// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <bit>
#include <cstring>
#include "IManagerForApplication.h"
#include "IAsyncContext.h"
#include "IAuthorizationRequest.h"

namespace skyline::service::account {
    IManagerForApplication::IManagerForApplication(const DeviceState &state, ServiceManager &manager, UserId userId,
                                                   std::shared_ptr<std::vector<UserId>> openedUsers)
        : userId(userId), openedUsers(std::move(openedUsers)), BaseService(state, manager) {}

    u64 IManagerForApplication::GetNetworkServiceAccountId() const {
        return userId.upper ^ std::rotl(userId.lower, 1);
    }

    Result IManagerForApplication::WriteIdTokenCache(ipc::IpcRequest &request, ipc::IpcResponse &response, size_t size) {
        if (size == 0) {
            response.Push<u32>(0);
            return {};
        }

        span<u8> output;
        try {
            output = request.outputBuf.at(0);
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }

        if (output.size() < size)
            return result::InvalidBufferSize;

        std::memset(output.data(), 0, size);
        response.Push(static_cast<u32>(size));
        return {};
    }

    Result IManagerForApplication::CheckAvailability(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // The local account exists and is usable. This command has no output value.
        return {};
    }

    Result IManagerForApplication::GetAccountId(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u64>(GetNetworkServiceAccountId());
        return {};
    }

    Result IManagerForApplication::EnsureIdTokenCacheAsync(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Matches Eden's completed async compatibility object: no Nintendo credential is fabricated.
        manager.RegisterService(SRVREG(IAsyncContext), session, response);
        return {};
    }

    Result IManagerForApplication::LoadIdTokenCacheDeprecated(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // Pre-19.x compatibility path. No real Nintendo token exists in Strato.
        return WriteIdTokenCache(request, response, 0);
    }

    Result IManagerForApplication::LoadIdTokenCache(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        // HOS 19.x split this from command 3. Eden exposes a fixed zeroed cache rather than inventing credentials.
        return WriteIdTokenCache(request, response, ModernIdTokenCacheSize);
    }

    Result IManagerForApplication::CreateAuthorizationRequest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        manager.RegisterService(SRVREG(IAuthorizationRequest), session, response);
        return {};
    }

    Result IManagerForApplication::StoreOpenContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        openedUsers->clear();
        openedUsers->push_back(userId);
        return {};
    }

    Result IManagerForApplication::GetNintendoAccountUserResourceCacheForApplication(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        span<u8> output;
        try {
            output = request.outputBuf.at(0);
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }

        if (output.size() < NasUserBaseForApplicationSize)
            return result::InvalidBufferSize;

        std::memset(output.data(), 0, NasUserBaseForApplicationSize);
        if (request.outputBuf.size() > 1 && !request.outputBuf[1].empty())
            std::memset(request.outputBuf[1].data(), 0, request.outputBuf[1].size());

        response.Push<u64>(GetNetworkServiceAccountId());
        return {};
    }
}
