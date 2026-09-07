// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IDatabaseService.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include <common/uuid.h>
#include <common/utils.h>

namespace skyline::service::mii {
    namespace {
        constexpr u32 DefaultSourceFlag{1U << 1};
        constexpr u32 DefaultMiiCount{6};
        constexpr Result InvalidArgument{static_cast<u32>(0x27E)};

        struct CharInfo {
            std::array<u8, 0x10> createId{};
            std::array<char16_t, 10> name{};
            u16 nullTerminator{};
            u8 fontRegion{};
            u8 favoriteColor{};
            u8 gender{};
            u8 height{};
            u8 build{};
            u8 type{};
            u8 regionMove{};
            u8 facelineType{};
            u8 facelineColor{};
            u8 facelineWrinkle{};
            u8 facelineMake{};
            u8 hairType{};
            u8 hairColor{};
            u8 hairFlip{};
            u8 eyeType{};
            u8 eyeColor{};
            u8 eyeScale{};
            u8 eyeAspect{};
            u8 eyeRotate{};
            u8 eyeX{};
            u8 eyeY{};
            u8 eyebrowType{};
            u8 eyebrowColor{};
            u8 eyebrowScale{};
            u8 eyebrowAspect{};
            u8 eyebrowRotate{};
            u8 eyebrowX{};
            u8 eyebrowY{};
            u8 noseType{};
            u8 noseScale{};
            u8 noseY{};
            u8 mouthType{};
            u8 mouthColor{};
            u8 mouthScale{};
            u8 mouthAspect{};
            u8 mouthY{};
            u8 beardColor{};
            u8 beardType{};
            u8 mustacheType{};
            u8 mustacheScale{};
            u8 mustacheY{};
            u8 glassType{};
            u8 glassColor{};
            u8 glassScale{};
            u8 glassY{};
            u8 moleType{};
            u8 moleScale{};
            u8 moleX{};
            u8 moleY{};
            u8 padding{};
        };
        static_assert(sizeof(CharInfo) == 0x58);

        struct CharInfoElement {
            CharInfo charInfo{};
            u32 source{};
        };
        static_assert(sizeof(CharInfoElement) == 0x5C);

        constexpr std::array<std::u16string_view, DefaultMiiCount> DefaultNames{
            u"Player", u"Mario", u"Luigi", u"Peach", u"Link", u"Samus"
        };

        void SetCreateId(CharInfo &info) {
            static_assert(sizeof(UUID) == 0x10);
            const auto uuid{UUID::GenerateUuidV4()};
            std::memcpy(info.createId.data(), &uuid, sizeof(uuid));
        }

        void SetName(CharInfo &info, std::u16string_view name) {
            info.name.fill(0);
            std::copy_n(name.begin(), std::min(name.size(), info.name.size()), info.name.begin());
        }

        CharInfo MakeDefaultMii(u32 index) {
            CharInfo info{};
            SetCreateId(info);
            SetName(info, DefaultNames.at(index));

            // These values mirror the compact six-Mii database used by the working APK.
            info.fontRegion = 0;
            info.favoriteColor = static_cast<u8>(index);
            info.gender = static_cast<u8>(index & 1U);
            info.height = 0x40;
            info.build = 0x40;
            info.type = 0;
            info.regionMove = 0;
            info.facelineType = 0;
            info.facelineColor = 0;
            info.facelineWrinkle = 0;
            info.facelineMake = 0;
            info.hairType = static_cast<u8>(0x21 + index);
            info.hairColor = static_cast<u8>(index + 1);
            info.hairFlip = 0;
            info.eyeType = static_cast<u8>(index);
            info.eyeColor = 0;
            info.eyeScale = 4;
            info.eyeAspect = 3;
            info.eyeRotate = 4;
            info.eyeX = 2;
            info.eyeY = 12;
            info.eyebrowType = static_cast<u8>(index);
            info.eyebrowColor = 0;
            info.eyebrowScale = 4;
            info.eyebrowAspect = 3;
            info.eyebrowRotate = 6;
            info.eyebrowX = 2;
            info.eyebrowY = 9;
            info.noseType = static_cast<u8>(index % 3U);
            info.noseScale = 4;
            info.noseY = 9;
            info.mouthType = static_cast<u8>(index & 3U);
            info.mouthColor = 0;
            info.mouthScale = 4;
            info.mouthAspect = 3;
            info.mouthY = 13;
            info.beardColor = 0;
            info.beardType = 0;
            info.mustacheType = 0;
            info.mustacheScale = 4;
            info.mustacheY = 10;
            info.glassType = info.noseType;
            info.glassColor = 0;
            info.glassScale = 4;
            info.glassY = 10;
            info.moleType = 0;
            info.moleScale = 4;
            info.moleX = 8;
            info.moleY = 15;
            info.padding = 0;
            return info;
        }

        CharInfo MakeRandomMii(u32 age, u32 gender, u32 race) {
            // Start from one of the known-valid presets and vary only fields whose ranges are known.
            // This keeps the returned CharInfo valid while matching the working APK's random-Mii ABI.
            auto info{MakeDefaultMii(util::RandomNumber<u32>(0, DefaultMiiCount - 1))};
            SetName(info, u"Random");
            info.gender = static_cast<u8>(gender == 2 ? util::RandomNumber<u32>(0, 1) : gender);
            info.favoriteColor = static_cast<u8>(util::RandomNumber<u32>(0, 5));

            if (age == 0)
                info.height = static_cast<u8>(util::RandomNumber<u32>(0x20, 0x48));
            else if (age == 2)
                info.height = static_cast<u8>(util::RandomNumber<u32>(0x30, 0x50));

            if (race == 0)
                info.facelineColor = 4;
            else if (race == 2)
                info.facelineColor = 2;

            return info;
        }
    }

    IDatabaseService::IDatabaseService(const DeviceState &state, ServiceManager &manager, u32 databaseType)
        : BaseService(state, manager), databaseType(databaseType) {}

    Result IDatabaseService::IsUpdated(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        request.Pop<u32>();
        response.Push<u8>(0);
        return {};
    }

    Result IDatabaseService::IsFullDatabase(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(0);
        return {};
    }

    Result IDatabaseService::GetCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto sourceFlag{request.Pop<u32>()};
        response.Push<u32>((sourceFlag & DefaultSourceFlag) ? DefaultMiiCount : 0);
        return {};
    }

    Result IDatabaseService::Get(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto sourceFlag{request.Pop<u32>()};
        u32 count{};

        if ((sourceFlag & DefaultSourceFlag) && !request.outputBuf.empty()) {
            auto &output{request.outputBuf.at(0)};
            const auto capacity{output.size() / sizeof(CharInfoElement)};
            count = static_cast<u32>(std::min<size_t>(DefaultMiiCount, capacity));

            for (u32 index{}; index < count; index++) {
                CharInfoElement element{
                    .charInfo = MakeDefaultMii(index),
                    .source = 1,
                };
                std::memcpy(output.data() + index * sizeof(CharInfoElement), &element, sizeof(element));
            }
        }

        response.Push<u32>(count);
        return {};
    }

    Result IDatabaseService::Get1(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto sourceFlag{request.Pop<u32>()};
        u32 count{};

        if ((sourceFlag & DefaultSourceFlag) && !request.outputBuf.empty()) {
            auto &output{request.outputBuf.at(0)};
            const auto capacity{output.size() / sizeof(CharInfo)};
            count = static_cast<u32>(std::min<size_t>(DefaultMiiCount, capacity));

            for (u32 index{}; index < count; index++) {
                const auto info{MakeDefaultMii(index)};
                std::memcpy(output.data() + index * sizeof(CharInfo), &info, sizeof(info));
            }
        }

        response.Push<u32>(count);
        return {};
    }

    Result IDatabaseService::BuildRandom(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto age{request.Pop<u32>()};
        const auto gender{request.Pop<u32>()};
        const auto race{request.Pop<u32>()};

        if (age > 3 || gender > 2 || race > 3)
            return InvalidArgument;

        response.Push(MakeRandomMii(age, gender, race));
        return {};
    }

    Result IDatabaseService::BuildDefault(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto index{request.Pop<u32>()};
        if (index >= DefaultMiiCount)
            return InvalidArgument;

        response.Push(MakeDefaultMii(index));
        return {};
    }

    Result IDatabaseService::SetInterfaceVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        interfaceVersion = request.Pop<u32>();
        return {};
    }

    Result IDatabaseService::DeleteFile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }
}
