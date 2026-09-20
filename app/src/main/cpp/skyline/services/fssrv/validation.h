// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>
#include "types.h"

namespace skyline::service::fssrv {
    constexpr size_t FspPathSize{0x301};

    constexpr bool ValidateRange(i64 offset, i64 size, size_t extent) {
        if (offset < 0 || size < 0)
            return false;

        const auto unsignedOffset{static_cast<u64>(offset)};
        const auto unsignedSize{static_cast<u64>(size)};
        if (unsignedOffset > std::numeric_limits<size_t>::max() || unsignedSize > std::numeric_limits<size_t>::max())
            return false;

        const auto checkedOffset{static_cast<size_t>(unsignedOffset)};
        const auto checkedSize{static_cast<size_t>(unsignedSize)};
        return checkedOffset <= extent && checkedSize <= extent - checkedOffset;
    }

    constexpr std::optional<size_t> ToSize(i64 size) {
        if (size < 0 || static_cast<u64>(size) > std::numeric_limits<size_t>::max())
            return std::nullopt;
        return static_cast<size_t>(size);
    }

    constexpr bool IsValidSaveDataSpaceId(SaveDataSpaceId spaceId) {
        switch (spaceId) {
            case SaveDataSpaceId::System:
            case SaveDataSpaceId::User:
            case SaveDataSpaceId::SdSystem:
            case SaveDataSpaceId::Temporary:
            case SaveDataSpaceId::SdCache:
            case SaveDataSpaceId::ProperSystem:
                return true;
            default:
                return false;
        }
    }

    constexpr bool IsValidSaveDataType(SaveDataType type) {
        return static_cast<u8>(type) <= static_cast<u8>(SaveDataType::SystemBcat);
    }

    constexpr bool IsValidSaveDataRank(SaveDataRank rank) {
        return rank == SaveDataRank::Primary || rank == SaveDataRank::Secondary;
    }

    inline std::optional<std::string> ReadPath(span<u8> buffer) {
        if (buffer.empty() || buffer.size() > FspPathSize)
            return std::nullopt;

        const auto terminator{std::find(buffer.begin(), buffer.end(), 0)};
        if (terminator == buffer.end())
            return std::nullopt;

        std::string path(reinterpret_cast<const char *>(buffer.data()), static_cast<size_t>(terminator - buffer.begin()));
        if (path.empty() || path.find('\\') != std::string::npos || path.starts_with("//"))
            return std::nullopt;

        const bool absolute{path.front() == '/'};
        std::string normalized{absolute ? "/" : ""};
        size_t position{absolute ? 1U : 0U};
        bool first{true};

        while (position < path.size()) {
            const auto separator{path.find('/', position)};
            const auto component{path.substr(position, separator - position)};
            if (component.empty() || component == "." || component == "..")
                return std::nullopt;

            if (!first)
                normalized += '/';
            normalized += component;
            first = false;

            if (separator == std::string::npos)
                break;
            position = separator + 1;
            if (position == path.size())
                break;
        }

        return normalized;
    }
}
