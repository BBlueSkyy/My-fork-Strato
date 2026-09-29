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
        constexpr std::array<char, 4> DatabaseMagic{'S', 'M', 'D', 'B'};
        constexpr u32 DatabaseVersion{1};
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

    s32 MiiDatabase::FindIndex(const std::array<u8, 0x10> &createId) const {
        std::scoped_lock lock{mutex};
        const auto entry{std::find_if(entries.begin(), entries.end(), [&](const CharInfo &candidate) {
            return candidate.createId == createId;
        })};

        if (entry == entries.end())
            return -1;

        return static_cast<s32>(std::distance(entries.begin(), entry));
    }

    MiiDatabase::AppendResult MiiDatabase::Append(const CharInfo &charInfo) {
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
