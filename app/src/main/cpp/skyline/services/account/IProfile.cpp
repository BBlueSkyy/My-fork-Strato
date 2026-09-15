// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cstring>
#include <os.h>
#include <vfs/os_backing.h>
#include <fcntl.h>
#include <common/settings.h>
#include "IProfile.h"

namespace skyline::service::account {
    namespace {
        struct AccountUserData {
            u32 unknown{};
            u32 iconId{};
            u8 iconBackgroundColorId{};
            std::array<u8, 0x7> reserved0{};
            std::array<u8, 0x10> miiId{};
            std::array<u8, 0x60> reserved1{};
        };
        static_assert(sizeof(AccountUserData) == 0x80);

        struct AccountProfileBase {
            UserId uid{};
            u64 lastEditTimestamp{};
            std::array<char, 0x20> nickname{};
        };
        static_assert(sizeof(AccountProfileBase) == 0x38);
    }

    IProfile::IProfile(const DeviceState &state, ServiceManager &manager, const UserId &userId) : BaseService(state, manager), userId(userId) {}

    Result IProfile::Get(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        span<u8> output;
        try {
            output = request.outputBuf.at(0);
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }

        if (output.size() < sizeof(AccountUserData))
            return result::InvalidBufferSize;

        auto &userData{output.as<AccountUserData>()};
        userData = {};
        userData.iconId = 1;
        userData.iconBackgroundColorId = 1;

        return GetBase(session, request, response);
    }

    Result IProfile::GetBase(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        AccountProfileBase accountProfileBase{
            .uid = userId,
        };

        const size_t usernameSize{std::min(accountProfileBase.nickname.size() - 1, (*state.settings->usernameValue).size())};
        std::memcpy(accountProfileBase.nickname.data(), (*state.settings->usernameValue).data(), usernameSize);

        response.Push(accountProfileBase);
        return {};
    }

    Result IProfile::GetImageSize(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto profileImageIcon{GetProfilePicture()};
        response.Push(static_cast<u32>(profileImageIcon->size));
        return {};
    }

    Result IProfile::LoadImage(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto profileImageIcon{GetProfilePicture()};

        span<u8> output;
        try {
            output = request.outputBuf.at(0);
        } catch (const std::out_of_range &) {
            return result::InvalidInputBuffer;
        }

        if (output.size() < profileImageIcon->size)
            return result::InvalidBufferSize;

        profileImageIcon->Read(output.first(profileImageIcon->size), 0);
        response.Push(static_cast<u32>(profileImageIcon->size));
        return {};
    }

    std::shared_ptr<vfs::Backing> IProfile::GetProfilePicture() {
        const std::string profilePicturePath{*state.settings->profilePictureValue};
        const int fd{open(profilePicturePath.c_str(), O_RDONLY)};
        if (fd < 0)
            return state.os->assetFileSystem->OpenFile("profile_picture.jpeg");
        return std::make_shared<vfs::OsBacking>(fd, true);
    }
}
