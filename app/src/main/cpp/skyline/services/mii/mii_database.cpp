// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "mii_database.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

#include <common/utils.h>
#include <os.h>

namespace skyline::service::mii {
    namespace {
        bool IsValidCreateId(const std::array<u8, 0x10> &createId) {
            const bool nonZero{std::any_of(createId.begin(), createId.end(), [](u8 value) { return value != 0; })};
            return nonZero && (createId[8] & 0xC0U) == 0x80U;
        }

        bool IsValidNickname(const CharInfo &info) {
            std::array<char16_t, 11> characters{};
            std::copy(info.name.begin(), info.name.end(), characters.begin());
            characters.back() = static_cast<char16_t>(info.nullTerminator);

            for (size_t index{}; index < characters.size(); index++) {
                const auto value{static_cast<u16>(characters[index])};
                if (value >= 0xD800 && value <= 0xDBFF) {
                    if (++index >= characters.size())
                        return false;
                    const auto low{static_cast<u16>(characters[index])};
                    if (low < 0xDC00 || low > 0xDFFF)
                        return false;
                } else if (value >= 0xDC00 && value <= 0xDFFF) {
                    return false;
                }
            }

            return true;
        }


        constexpr std::array<char, 4> DatabaseMagic{'S', 'M', 'D', 'B'};
        constexpr u32 DatabaseVersion{1};
    }

    bool IsValidCharInfo(const CharInfo &info) {
        if (!IsValidCreateId(info.createId) || !IsValidNickname(info))
            return false;

        return info.fontRegion <= 3 &&
               info.favoriteColor <= 11 &&
               info.gender <= 1 &&
               info.height <= 0x7F &&
               info.build <= 0x7F &&
               info.type <= 1 &&
               info.regionMove <= 3 &&
               info.facelineType <= 11 &&
               info.facelineColor <= 9 &&
               info.facelineWrinkle <= 11 &&
               info.facelineMake <= 11 &&
               info.hairType <= 131 &&
               info.hairColor <= 99 &&
               info.hairFlip <= 1 &&
               info.eyeType <= 59 &&
               info.eyeColor <= 99 &&
               info.eyeScale <= 7 &&
               info.eyeAspect <= 6 &&
               info.eyeRotate <= 7 &&
               info.eyeX <= 12 &&
               info.eyeY <= 18 &&
               info.eyebrowType <= 23 &&
               info.eyebrowColor <= 99 &&
               info.eyebrowScale <= 8 &&
               info.eyebrowAspect <= 6 &&
               info.eyebrowRotate <= 11 &&
               info.eyebrowX <= 12 &&
               info.eyebrowY <= 18 &&
               info.noseType <= 17 &&
               info.noseScale <= 8 &&
               info.noseY <= 18 &&
               info.mouthType <= 35 &&
               info.mouthColor <= 99 &&
               info.mouthScale <= 8 &&
               info.mouthAspect <= 6 &&
               info.mouthY <= 18 &&
               info.beardColor <= 99 &&
               info.beardType <= 5 &&
               info.mustacheType <= 5 &&
               info.mustacheScale <= 8 &&
               info.mustacheY <= 16 &&
               info.glassType <= 19 &&
               info.glassColor <= 99 &&
               info.glassScale <= 7 &&
               info.glassY <= 20 &&
               info.moleType <= 1 &&
               info.moleScale <= 8 &&
               info.moleX <= 16 &&
               info.moleY <= 30;
    }

    MiiDatabase::MiiDatabase(const DeviceState &state)
        : backingPath(std::filesystem::path(state.os->publicAppFilesPath) /
                      "switch" / "nand" / "system" / "save" / "8000000000000030" /
                      "MiiDatabase.strato") {
        Load();
    }

    void MiiDatabase::GenerateCreateId(std::array<u8, 0x10> &createId) {
        util::FillRandomBytes(std::span<u8>{createId.data(), createId.size()});
        createId[6] = static_cast<u8>((createId[6] & 0x0FU) | 0x40U);
        createId[8] = static_cast<u8>((createId[8] & 0x3FU) | 0x80U);
    }

    bool MiiDatabase::ContainsCreateIdLocked(const std::array<u8, 0x10> &createId) const {
        return std::any_of(entries.begin(), entries.end(), [&](const CharInfo &entry) {
            return entry.createId == createId;
        });
    }

    bool MiiDatabase::Load() {
        std::scoped_lock lock{mutex};

        std::ifstream file{backingPath, std::ios::binary};
        if (!file)
            return false;

        FileHeader header{};
        file.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (!file || header.magic != DatabaseMagic || header.version != DatabaseVersion ||
            header.count > MaxMiiCount)
            return false;

        std::vector<CharInfo> loaded(header.count);
        if (!loaded.empty()) {
            file.read(reinterpret_cast<char *>(loaded.data()),
                      static_cast<std::streamsize>(loaded.size() * sizeof(CharInfo)));
            if (!file)
                return false;
        }

        char trailing{};
        if (file.read(&trailing, 1))
            return false;

        entries = std::move(loaded);
        return true;
    }

    bool MiiDatabase::SaveLocked() const {
        std::error_code error;
        std::filesystem::create_directories(backingPath.parent_path(), error);
        if (error)
            return false;

        auto temporaryPath{backingPath};
        temporaryPath += ".tmp";

        {
            std::ofstream file{temporaryPath, std::ios::binary | std::ios::trunc};
            if (!file)
                return false;

            FileHeader header{};
            header.magic = DatabaseMagic;
            header.version = DatabaseVersion;
            header.count = static_cast<u32>(entries.size());

            file.write(reinterpret_cast<const char *>(&header), sizeof(header));
            if (!entries.empty()) {
                file.write(reinterpret_cast<const char *>(entries.data()),
                           static_cast<std::streamsize>(entries.size() * sizeof(CharInfo)));
            }

            file.flush();
            if (!file)
                return false;
        }

        std::filesystem::rename(temporaryPath, backingPath, error);
        if (!error)
            return true;

        std::error_code removeError;
        std::filesystem::remove(backingPath, removeError);
        error.clear();
        std::filesystem::rename(temporaryPath, backingPath, error);
        if (error) {
            std::filesystem::remove(temporaryPath, removeError);
            return false;
        }

        return true;
    }

    u64 MiiDatabase::GetUpdateCounter() const {
        std::scoped_lock lock{mutex};
        return updateCounter;
    }

    u32 MiiDatabase::GetCount() const {
        std::scoped_lock lock{mutex};
        return static_cast<u32>(entries.size());
    }

    bool MiiDatabase::IsFull() const {
        std::scoped_lock lock{mutex};
        return entries.size() >= MaxMiiCount;
    }

    std::vector<CharInfo> MiiDatabase::Snapshot() const {
        std::scoped_lock lock{mutex};
        return entries;
    }

    std::optional<CharInfo> MiiDatabase::FindByCreateId(const std::array<u8, 0x10> &createId) const {
        std::scoped_lock lock{mutex};
        const auto entry{std::find_if(entries.begin(), entries.end(), [&](const CharInfo &candidate) {
            return candidate.createId == createId;
        })};

        if (entry == entries.end())
            return std::nullopt;

        return *entry;
    }

    i32 MiiDatabase::FindIndex(const std::array<u8, 0x10> &createId) const {
        std::scoped_lock lock{mutex};
        const auto entry{std::find_if(entries.begin(), entries.end(), [&](const CharInfo &candidate) {
            return candidate.createId == createId;
        })};

        if (entry == entries.end())
            return -1;

        return static_cast<i32>(std::distance(entries.begin(), entry));
    }

    MiiDatabase::AppendResult MiiDatabase::Append(const CharInfo &charInfo) {
        if (!IsValidCharInfo(charInfo))
            return AppendResult::InvalidCharInfo;

        if (charInfo.type == 1)
            return AppendResult::InvalidSpecial;

        std::scoped_lock lock{mutex};
        if (entries.size() >= MaxMiiCount)
            return AppendResult::Full;

        CharInfo storedInfo{charInfo};
        do {
            GenerateCreateId(storedInfo.createId);
        } while (ContainsCreateIdLocked(storedInfo.createId));

        entries.emplace_back(storedInfo);
        updateCounter++;
        SaveLocked();
        return AppendResult::Success;
    }
}
