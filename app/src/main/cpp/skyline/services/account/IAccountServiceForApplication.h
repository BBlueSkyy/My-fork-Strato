// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <services/serviceman.h>

namespace skyline {
    namespace service::account {
        namespace result {
            constexpr Result NullArgument(124, 20);
            constexpr Result InvalidArgument(124, 22);
            constexpr Result NullInputBuffer(124, 30);
            constexpr Result InvalidBufferSize(124, 31);
            constexpr Result InvalidInputBuffer(124, 32);
            constexpr Result ApplicationInfoAlreadyInitialized(124, 41);
            constexpr Result UserNotFound(124, 100);
        }

        struct UserId {
            u64 upper;
            u64 lower;

            constexpr bool operator==(const UserId &userId) const = default;
            constexpr bool operator!=(const UserId &userId) const = default;
        };
        static_assert(sizeof(UserId) == 0x10);

        class IAccountServiceForApplication : public BaseService {
          private:
            static constexpr size_t SaveDataThumbnailSize{0x24000};

            std::shared_ptr<std::vector<UserId>> openedUsers;
            std::vector<u8> saveDataThumbnail;
            bool applicationInfoInitialized{};
            u64 applicationId{};

            Result WriteUserList(span<u8> buffer, const std::vector<UserId> &userIds);
            Result ValidateUserId(const UserId &userId) const;
            Result InitializeApplicationInfoCommon(ipc::IpcRequest &request, bool hasPidPlaceholder);

          public:
            IAccountServiceForApplication(const DeviceState &state, ServiceManager &manager);

            Result GetUserCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result GetUserExistence(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result ListAllUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result ListOpenUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result GetLastOpenedUser(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result GetProfile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result IsUserRegistrationRequestPermitted(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result TrySelectUserWithoutInteraction(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result InitializeApplicationInfoV0(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result GetBaasAccountManagerForApplication(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result StoreSaveDataThumbnail(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result ClearSaveDataThumbnail(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result LoadOpenContext(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result ListOpenContextStoredUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result InitializeApplicationInfo(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result ListQualifiedUsers(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result IsUserAccountSwitchLocked(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);
            Result InitializeApplicationInfoV2(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response);

            SERVICE_DECL(
                SFUNC(0x0, IAccountServiceForApplication, GetUserCount),
                SFUNC(0x1, IAccountServiceForApplication, GetUserExistence),
                SFUNC(0x2, IAccountServiceForApplication, ListAllUsers),
                SFUNC(0x3, IAccountServiceForApplication, ListOpenUsers),
                SFUNC(0x4, IAccountServiceForApplication, GetLastOpenedUser),
                SFUNC(0x5, IAccountServiceForApplication, GetProfile),
                SFUNC(0x32, IAccountServiceForApplication, IsUserRegistrationRequestPermitted),
                SFUNC(0x33, IAccountServiceForApplication, TrySelectUserWithoutInteraction),
                SFUNC(0x34, IAccountServiceForApplication, TrySelectUserWithoutInteraction),
                SFUNC(0x3C, IAccountServiceForApplication, ListOpenContextStoredUsers),
                SFUNC(0x64, IAccountServiceForApplication, InitializeApplicationInfoV0),
                SFUNC(0x65, IAccountServiceForApplication, GetBaasAccountManagerForApplication),
                SFUNC(0x6E, IAccountServiceForApplication, StoreSaveDataThumbnail),
                SFUNC(0x6F, IAccountServiceForApplication, ClearSaveDataThumbnail),
                SFUNC(0x82, IAccountServiceForApplication, LoadOpenContext),
                SFUNC(0x83, IAccountServiceForApplication, ListOpenContextStoredUsers),
                SFUNC(0x8C, IAccountServiceForApplication, InitializeApplicationInfo),
                SFUNC(0x8D, IAccountServiceForApplication, ListQualifiedUsers),
                SFUNC(0x96, IAccountServiceForApplication, IsUserAccountSwitchLocked),
                SFUNC(0xA0, IAccountServiceForApplication, InitializeApplicationInfoV2)
            )
        };
    }

    namespace constant {
        constexpr service::account::UserId DefaultUserId{0x0000000000000001, 0x0000000000000000};
    }
}
