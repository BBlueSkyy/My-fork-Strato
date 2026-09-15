// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cstring>
#include <kernel/types/KProcess.h>
#include <loader/loader.h>
#include "IManagerForApplication.h"
#include "IProfile.h"
#include "IAccountServiceForApplication.h"

namespace skyline::service::account {
    IAccountServiceForApplication::IAccountServiceForApplication(const DeviceState &state, ServiceManager &manager)
        : BaseService(state, manager), openedUsers(std::make_shared<std::vector<UserId>>()) {}

    Result IAccountServiceForApplication::ValidateUserId(const UserId &userId) const {
        if (userId == UserId{})
            return result::NullArgument;
        if (userId != constant::DefaultUserId)
            return result::UserNotFound;
        return {};
    }

    Result IAccountServiceForApplication::InitializeApplicationInfoCommon(ipc::IpcRequest &request, bool hasPidPlaceholder) {
        if (hasPidPlaceholder) {
            [[maybe_unused]] const auto pidPlaceholder{request.Pop<u64>()};
        }

        if (applicationInfoInitialized)
            return result::ApplicationInfoAlreadyInitialized;
        if (!state.process || state.process->npdm.aci0.programId == 0)
            return result::InvalidArgument;

        applicationId = state.process->npdm.aci0.programId;
        applicationInfoInitialized = true;
        return {};
    }

    Result IAccountServiceForApplication::GetUserCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<i32>(1);
        return {};
    }

    Result IAccountServiceForApplication::GetUserExistence(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto userId{request.Pop<UserId>()};
        if (userId == UserId{})
            return result::NullArgument;

        response.Push<u8>(userId == constant::DefaultUserId);
        return {};
    }

    Result IAccountServiceForApplication::ListAllUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        try {
            return WriteUserList(request.outputBuf.at(0), {constant::DefaultUserId});
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }
    }

    Result IAccountServiceForApplication::ListOpenUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        try {
            return WriteUserList(request.outputBuf.at(0), {constant::DefaultUserId});
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }
    }

    Result IAccountServiceForApplication::WriteUserList(span<u8> buffer, const std::vector<UserId> &userIds) {
        if (buffer.size() % sizeof(UserId) != 0)
            return result::InvalidBufferSize;

        if (!buffer.empty())
            std::memset(buffer.data(), 0, buffer.size());

        auto outputUserIds{buffer.cast<UserId>()};
        const size_t count{std::min(outputUserIds.size(), userIds.size())};
        for (size_t index{}; index < count; index++)
            outputUserIds[index] = userIds[index];

        return {};
    }

    Result IAccountServiceForApplication::GetLastOpenedUser(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push(constant::DefaultUserId);
        return {};
    }

    Result IAccountServiceForApplication::InitializeApplicationInfoV0(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return InitializeApplicationInfoCommon(request, true);
    }

    Result IAccountServiceForApplication::GetProfile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto userId{request.Pop<UserId>()};
        const auto validation{ValidateUserId(userId)};
        if (validation)
            return validation;

        manager.RegisterService(std::make_shared<IProfile>(state, manager, userId), session, response);
        return {};
    }

    Result IAccountServiceForApplication::IsUserRegistrationRequestPermitted(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto pidReserved{request.Pop<u64>()};
        response.Push<u8>(false);
        return {};
    }

    Result IAccountServiceForApplication::TrySelectUserWithoutInteraction(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        [[maybe_unused]] const auto isNetworkServiceAccountRequired{request.Pop<u8>()};
        response.Push(constant::DefaultUserId);
        return {};
    }

    Result IAccountServiceForApplication::GetBaasAccountManagerForApplication(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto userId{request.Pop<UserId>()};
        const auto validation{ValidateUserId(userId)};
        if (validation)
            return validation;

        manager.RegisterService(std::make_shared<IManagerForApplication>(state, manager, userId, openedUsers), session, response);
        return {};
    }

    Result IAccountServiceForApplication::InitializeApplicationInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return InitializeApplicationInfoCommon(request, true);
    }

    Result IAccountServiceForApplication::ListQualifiedUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        try {
            return WriteUserList(request.outputBuf.at(0), {constant::DefaultUserId});
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }
    }

    Result IAccountServiceForApplication::StoreSaveDataThumbnail(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto userId{request.Pop<UserId>()};
        const auto validation{ValidateUserId(userId)};
        if (validation)
            return validation;

        const u64 currentApplicationId{applicationInfoInitialized ? applicationId : (state.process ? state.process->npdm.aci0.programId : 0)};
        if (currentApplicationId == 0)
            return result::InvalidArgument;

        span<u8> input;
        try {
            input = request.inputBuf.at(0);
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }

        if (input.size() != SaveDataThumbnailSize)
            return result::InvalidBufferSize;

        saveDataThumbnail.resize(input.size());
        std::memcpy(saveDataThumbnail.data(), input.data(), input.size());
        return {};
    }

    Result IAccountServiceForApplication::ClearSaveDataThumbnail(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto userId{request.Pop<UserId>()};
        const auto validation{ValidateUserId(userId)};
        if (validation)
            return validation;

        saveDataThumbnail.clear();
        return {};
    }

    Result IAccountServiceForApplication::LoadOpenContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto userId{request.Pop<UserId>()};
        const auto validation{ValidateUserId(userId)};
        if (validation)
            return validation;

        manager.RegisterService(std::make_shared<IManagerForApplication>(state, manager, userId, openedUsers), session, response);
        return {};
    }

    Result IAccountServiceForApplication::ListOpenContextStoredUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        try {
            return WriteUserList(request.outputBuf.at(0), *openedUsers);
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }
    }

    Result IAccountServiceForApplication::IsUserAccountSwitchLocked(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const bool isLocked{state.loader && state.loader->nacp && state.loader->nacp->nacpContents.userAccountSwitchLock != 0};
        response.Push<u8>(isLocked);
        return {};
    }

    Result IAccountServiceForApplication::InitializeApplicationInfoV2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return InitializeApplicationInfoCommon(request, false);
    }
}
