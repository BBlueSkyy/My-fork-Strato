// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "IDatabaseService.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include <common/utils.h>

namespace skyline::service::mii {
    namespace {
        constexpr u32 DefaultMiiCount{6};
        constexpr Result InvalidArgument{static_cast<u32>(0x27E)};
        constexpr Result BufferTooSmall{static_cast<u32>(0x47E)};
        constexpr Result NotUpdated{static_cast<u32>(0x67E)};
        constexpr Result NotFound{static_cast<u32>(0x87E)};
        constexpr Result DatabaseFull{static_cast<u32>(0xA7E)};
        constexpr Result InvalidCharInfo{static_cast<u32>((100U << 9U) | 126U)};
        constexpr Result InvalidOperationOnSpecialMii{static_cast<u32>((202U << 9U) | 126U)};

        constexpr std::array<std::u16string_view, DefaultMiiCount> DefaultNames{
            u"Player", u"Mario", u"Luigi", u"Peach", u"Link", u"Samus"
        };

        void SetCreateId(CharInfo &info) {
            util::FillRandomBytes(std::span<u8>{info.createId.data(), info.createId.size()});
            info.createId[8] = static_cast<u8>((info.createId[8] & 0x3FU) | 0x80U);
        }

        void SetName(CharInfo &info, std::u16string_view name) {
            info.name.fill(0);
            std::copy_n(name.begin(), std::min(name.size(), info.name.size()), info.name.begin());
        }

        CharInfo MakeDefaultMii(u32 index) {
            CharInfo info{};
            SetCreateId(info);
            SetName(info, DefaultNames.at(index));

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
            auto info{MakeDefaultMii(util::RandomNumber<u32>(0, DefaultMiiCount - 1))};

            SetCreateId(info);
            SetName(info, u"Random");

            if (gender == 2)
                gender = util::RandomNumber<u32>(0, 1);
            info.gender = static_cast<u8>(gender);

            if (age == 3)
                age = util::RandomNumber<u32>(0, 2);

            switch (age) {
                case 0:
                    info.height = static_cast<u8>(util::RandomNumber<u32>(0x20, 0x48));
                    info.build = static_cast<u8>(util::RandomNumber<u32>(0x1E, 0x48));
                    break;
                case 1:
                    info.height = static_cast<u8>(util::RandomNumber<u32>(0x38, 0x68));
                    info.build = static_cast<u8>(util::RandomNumber<u32>(0x28, 0x60));
                    break;
                case 2:
                    info.height = static_cast<u8>(util::RandomNumber<u32>(0x30, 0x60));
                    info.build = static_cast<u8>(util::RandomNumber<u32>(0x20, 0x5A));
                    break;
            }

            if (race == 3)
                race = util::RandomNumber<u32>(0, 2);

            switch (race) {
                case 0:
                    info.facelineColor = 6;
                    info.hairColor = 1;
                    break;
                case 1:
                    info.facelineColor = 0;
                    break;
                case 2:
                    info.facelineColor = 2;
                    info.eyeType = static_cast<u8>(util::RandomNumber<u32>(2, 7));
                    break;
            }

            info.favoriteColor = static_cast<u8>(util::RandomNumber<u32>(0, 11));
            info.hairFlip = static_cast<u8>(util::RandomNumber<u32>(0, 1));
            info.eyeScale = static_cast<u8>(util::RandomNumber<u32>(0, 7));
            info.eyeAspect = static_cast<u8>(util::RandomNumber<u32>(0, 6));
            info.eyeRotate = static_cast<u8>(util::RandomNumber<u32>(0, 7));
            info.eyeX = static_cast<u8>(util::RandomNumber<u32>(0, 12));
            info.eyeY = static_cast<u8>(util::RandomNumber<u32>(0, 18));
            info.eyebrowScale = static_cast<u8>(util::RandomNumber<u32>(0, 8));
            info.eyebrowAspect = static_cast<u8>(util::RandomNumber<u32>(0, 6));
            info.eyebrowRotate = static_cast<u8>(util::RandomNumber<u32>(0, 11));
            info.eyebrowX = static_cast<u8>(util::RandomNumber<u32>(0, 12));
            info.eyebrowY = static_cast<u8>(util::RandomNumber<u32>(3, 18));
            info.noseScale = static_cast<u8>(util::RandomNumber<u32>(0, 8));
            info.noseY = static_cast<u8>(util::RandomNumber<u32>(0, 18));
            info.mouthScale = static_cast<u8>(util::RandomNumber<u32>(0, 8));
            info.mouthAspect = static_cast<u8>(util::RandomNumber<u32>(0, 6));
            info.mouthY = static_cast<u8>(util::RandomNumber<u32>(0, 18));
            info.mustacheScale = static_cast<u8>(util::RandomNumber<u32>(0, 8));
            info.mustacheY = static_cast<u8>(util::RandomNumber<u32>(0, 16));
            info.glassScale = static_cast<u8>(util::RandomNumber<u32>(0, 7));
            info.glassY = static_cast<u8>(util::RandomNumber<u32>(0, 20));
            info.moleScale = static_cast<u8>(util::RandomNumber<u32>(0, 8));
            info.moleX = static_cast<u8>(util::RandomNumber<u32>(0, 16));
            info.moleY = static_cast<u8>(util::RandomNumber<u32>(0, 30));
            return info;
        }
    }

    IDatabaseService::IDatabaseService(const DeviceState &state, ServiceManager &manager, MiiDatabase &database, u32 databaseType)
        : BaseService(state, manager), database(database), databaseType(databaseType),
          updateCounter(database.GetUpdateCounter()) {}

    Result IDatabaseService::IsUpdated(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto sourceFlag{request.Pop<u32>()};
        bool updated{};

        if (sourceFlag & DatabaseSourceFlag) {
            const auto currentCounter{database.GetUpdateCounter()};
            updated = updateCounter != currentCounter;
            updateCounter = currentCounter;
        }

        response.Push<u8>(updated);
        return {};
    }

    Result IDatabaseService::IsFullDatabase(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        response.Push<u8>(database.IsFull());
        return {};
    }

    Result IDatabaseService::GetCount(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto sourceFlag{request.Pop<u32>()};
        u32 count{};
        if (sourceFlag & DatabaseSourceFlag)
            count += database.GetCount();
        if (sourceFlag & DefaultSourceFlag)
            count += DefaultMiiCount;
        response.Push<u32>(count);
        return {};
    }

    Result IDatabaseService::Get(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto sourceFlag{request.Pop<u32>()};
        auto &output{request.outputBuf.at(0)};
        const auto capacity{output.size() / sizeof(CharInfoElement)};
        size_t count{};

        if (sourceFlag & DatabaseSourceFlag) {
            const auto entries{database.Snapshot()};
            for (const auto &info : entries) {
                if (count >= capacity) {
                    response.Push<u32>(static_cast<u32>(count));
                    return BufferTooSmall;
                }

                const CharInfoElement element{
                    .charInfo = info,
                    .source = Source::Database,
                };
                std::memcpy(output.data() + count * sizeof(CharInfoElement), &element, sizeof(element));
                count++;
            }
        }

        if (sourceFlag & DefaultSourceFlag) {
            for (u32 index{}; index < DefaultMiiCount; index++) {
                if (count >= capacity) {
                    response.Push<u32>(static_cast<u32>(count));
                    return BufferTooSmall;
                }

                const CharInfoElement element{
                    .charInfo = MakeDefaultMii(index),
                    .source = Source::Default,
                };
                std::memcpy(output.data() + count * sizeof(CharInfoElement), &element, sizeof(element));
                count++;
            }
        }

        response.Push<u32>(static_cast<u32>(count));
        return {};
    }

    Result IDatabaseService::Get1(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto sourceFlag{request.Pop<u32>()};
        auto &output{request.outputBuf.at(0)};
        const auto capacity{output.size() / sizeof(CharInfo)};
        size_t count{};

        if (sourceFlag & DatabaseSourceFlag) {
            const auto entries{database.Snapshot()};
            for (const auto &info : entries) {
                if (count >= capacity) {
                    response.Push<u32>(static_cast<u32>(count));
                    return BufferTooSmall;
                }

                std::memcpy(output.data() + count * sizeof(CharInfo), &info, sizeof(info));
                count++;
            }
        }

        if (sourceFlag & DefaultSourceFlag) {
            for (u32 index{}; index < DefaultMiiCount; index++) {
                if (count >= capacity) {
                    response.Push<u32>(static_cast<u32>(count));
                    return BufferTooSmall;
                }

                const auto info{MakeDefaultMii(index)};
                std::memcpy(output.data() + count * sizeof(CharInfo), &info, sizeof(info));
                count++;
            }
        }

        response.Push<u32>(static_cast<u32>(count));
        return {};
    }

    Result IDatabaseService::UpdateLatest(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto oldCharInfo{request.Pop<CharInfo>()};
        const auto sourceFlag{request.Pop<u32>()};

        if (!(sourceFlag & DatabaseSourceFlag)) {
            response.Push(CharInfo{});
            return NotFound;
        }

        if (interfaceVersion >= 1 && !IsValidCharInfo(oldCharInfo)) {
            response.Push(CharInfo{});
            return InvalidCharInfo;
        }

        const auto storedInfo{database.FindByCreateId(oldCharInfo.createId)};
        if (!storedInfo || storedInfo->type != oldCharInfo.type) {
            response.Push(CharInfo{});
            return NotFound;
        }

        response.Push(*storedInfo);
        auto oldComparable{oldCharInfo};
        auto storedComparable{*storedInfo};
        oldComparable.padding = 0;
        storedComparable.padding = 0;
        if (std::memcmp(&oldComparable, &storedComparable, sizeof(CharInfo)) == 0)
            return NotUpdated;

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

    Result IDatabaseService::GetIndex(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto charInfo{request.Pop<CharInfo>()};
        if (!IsValidCharInfo(charInfo)) {
            response.Push<s32>(-1);
            return InvalidCharInfo;
        }

        const auto index{database.FindIndex(charInfo.createId)};
        response.Push<s32>(index);
        return index >= 0 ? Result{} : NotFound;
    }

    Result IDatabaseService::SetInterfaceVersion(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        interfaceVersion = request.Pop<u32>();
        return {};
    }

    Result IDatabaseService::DeleteFile(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        return {};
    }

    Result IDatabaseService::Append(type::KSession &session, ipc::IpcRequest &request, ipc::IpcResponse &response) {
        const auto charInfo{request.Pop<CharInfo>()};

        switch (database.Append(charInfo)) {
            case MiiDatabase::AppendResult::Success:
                updateCounter = database.GetUpdateCounter();
                return {};
            case MiiDatabase::AppendResult::Full:
                return DatabaseFull;
            case MiiDatabase::AppendResult::InvalidCharInfo:
                return InvalidCharInfo;
            case MiiDatabase::AppendResult::InvalidSpecial:
                return InvalidOperationOnSpecialMii;
        }

        return InvalidArgument;
    }
}
