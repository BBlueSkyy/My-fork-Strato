// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Contributors

#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <common.h>

namespace skyline::service::settings {
    // Values are indexed by their set:sys getter ID. The file is an emulator
    // format, not the Horizon system save. Commits become visible after rename.
    class SettingsStore {
        std::string path;
        std::mutex mutex;
        std::map<u32, std::vector<u8>> values;
        bool readable{true};

      public:
        const bool internetAllowed; //!< Android setting is an upper bound on guest connectivity.
        explicit SettingsStore(const DeviceState &state);
        ResultValue<std::vector<u8>> Get(u32 key, span<const u8> fallback);
        Result Set(u32 key, span<const u8> value);

        template<typename T>
        ResultValue<T> Get(u32 key, const T &fallback) {
            static_assert(std::is_trivially_copyable_v<T>);
            auto bytes{Get(key, span<const u8>(reinterpret_cast<const u8 *>(&fallback), sizeof(T)))};
            if (!bytes)
                return bytes.result;
            if (bytes->size() != sizeof(T))
                return Result{105, 263};
            T value;
            std::memcpy(&value, bytes->data(), sizeof(T));
            return value;
        }

        template<typename T>
        Result Set(u32 key, const T &value) {
            static_assert(std::is_trivially_copyable_v<T>);
            return Set(key, span<const u8>(reinterpret_cast<const u8 *>(&value), sizeof(T)));
        }
    };
}
