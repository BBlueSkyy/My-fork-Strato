// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <os.h>
#include <common/settings.h>
#include <kernel/results.h>
#include "settings_store.h"

namespace skyline::service::settings {
    namespace {
        constexpr std::array<char, 8> Magic{'S', 'T', 'R', 'S', 'E', 'T', '0', '1'};
        constexpr u32 MaxEntries{512};
        constexpr u32 MaxValueSize{0x10000};
        // Host I/O failure is surfaced as an emulator error, never Success.
        constexpr Result StorageFailure{1, 33};
    }

    SettingsStore::SettingsStore(const DeviceState &state)
        : path(state.os->privateAppFilesPath + "/system-settings.bin"), internetAllowed(*state.settings->isInternetEnabled) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            readable = access(path.c_str(), F_OK) != 0 && errno == ENOENT;
            if (!readable)
                LOGW("Unable to open system settings store");
            return;
        }
        std::array<char, 8> magic{};
        u32 count{};
        file.read(magic.data(), magic.size());
        file.read(reinterpret_cast<char *>(&count), sizeof(count));
        readable = file.good() && magic == Magic && count <= MaxEntries;
        for (u32 i{}; readable && i < count; ++i) {
            u32 key{}, size{};
            file.read(reinterpret_cast<char *>(&key), sizeof(key));
            file.read(reinterpret_cast<char *>(&size), sizeof(size));
            if (!file || size > MaxValueSize || values.contains(key)) {
                readable = false;
                break;
            }
            std::vector<u8> value(size);
            file.read(reinterpret_cast<char *>(value.data()), size);
            readable = file.good();
            values.emplace(key, std::move(value));
        }
        readable = readable && file.peek() == std::char_traits<char>::eof();
        if (!readable) {
            values.clear();
            LOGW("Invalid system settings store; preserving file and rejecting access");
        }
        if (readable) {
            auto wireless{Get<u8>(73, 1)};
            if (wireless && *wireless <= 1)
                state.settings->isInternetEnabled = internetAllowed && *wireless;
            auto language{Get<LanguageCode>(0, language::GetLanguageCode(*state.settings->systemLanguage))};
            if (language) {
                auto it{std::find(language::LanguageCodeList.begin(), language::LanguageCodeList.end(), *language)};
                if (it != language::LanguageCodeList.end())
                    state.settings->systemLanguage = static_cast<language::SystemLanguage>(it - language::LanguageCodeList.begin());
            }
            auto region{Get<i32>(57, static_cast<i32>(*state.settings->systemRegion))};
            if (region && *region >= -1 && *region <= 5)
                state.settings->systemRegion = static_cast<region::RegionCode>(*region);
        }

    }

    ResultValue<std::vector<u8>> SettingsStore::Get(u32 key, span<const u8> fallback) {
        std::scoped_lock lock{mutex};
        if (!readable)
            return StorageFailure;
        auto it{values.find(key)};
        if (it != values.end())
            return it->second;
        return std::vector<u8>(fallback.begin(), fallback.end());
    }

    Result SettingsStore::Set(u32 key, span<const u8> value) {
        std::scoped_lock lock{mutex};
        if (!readable)
            return StorageFailure;
        if (value.size() > MaxValueSize || (!values.contains(key) && values.size() >= MaxEntries))
            return kernel::result::InvalidArgument;
        auto next{values};
        next[key] = std::vector<u8>(value.begin(), value.end());
        const auto temporary{path + ".tmp"};
        FILE *file{std::fopen(temporary.c_str(), "wb")};
        if (!file)
            return StorageFailure;
        auto write = [file](const void *data, size_t size) {
            return !size || std::fwrite(data, 1, size, file) == size;
        };
        const auto count{static_cast<u32>(next.size())};
        bool ok{write(Magic.data(), Magic.size()) && write(&count, sizeof(count))};
        for (const auto &[id, bytes] : next) {
            const auto size{static_cast<u32>(bytes.size())};
            ok = ok && write(&id, sizeof(id)) && write(&size, sizeof(size)) && write(bytes.data(), bytes.size());
        }
        ok = ok && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
        if (std::fclose(file))
            ok = false;
        if (!ok || std::rename(temporary.c_str(), path.c_str())) {
            std::remove(temporary.c_str());
            return StorageFailure;
        }
        values.swap(next);
        return {};
    }
}
