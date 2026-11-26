// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <system_error>
#include <vfs/backing.h>
#include "results.h"

namespace skyline::service::fssrv {
    inline Result MapVfsError(const std::error_code &error) {
        if (!error)
            return {};
        if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
            return result::PathDoesNotExist;
        if (error == std::errc::file_exists)
            return result::PathAlreadyExists;
        if (error == std::errc::filename_too_long)
            return result::TooLongPath;
        if (error == std::errc::invalid_argument)
            return result::InvalidPath;
        if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted || error == std::errc::read_only_file_system)
            return result::PermissionDenied;
        if (error == std::errc::operation_not_supported)
            return result::NotImplemented;
        return result::UnexpectedFailure;
    }

    inline Result MapBackingError(const std::error_code &error, bool writeOperation = false) {
        if (!error)
            return {};
        if (error == std::errc::result_out_of_range || error == std::errc::value_too_large)
            return result::OutOfRange;
        if (error == std::errc::read_only_file_system)
            return result::WriteNotPermitted;
        if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
            return writeOperation ? result::WriteNotPermitted : result::ReadNotPermitted;
        if (error == std::errc::operation_not_supported)
            return result::NotImplemented;
        return result::UnexpectedFailure;
    }

    constexpr bool IsOpenModeValid(vfs::Backing::Mode mode) {
        constexpr u32 KnownModeMask{0x7};
        return (mode.raw & ~KnownModeMask) == 0 && (mode.read || mode.write) && (!mode.append || mode.write);
    }

    constexpr bool IsMutationAllowed(bool readOnly, vfs::Backing::Mode mode) {
        return !readOnly || (!mode.write && !mode.append);
    }

    constexpr bool IsDirectoryModeValid(u32 mode) {
        constexpr u32 EntryMask{0x3};
        constexpr u32 KnownModeMask{EntryMask | (1U << 31)};
        return (mode & EntryMask) != 0 && (mode & ~KnownModeMask) == 0;
    }

    constexpr size_t CalculateReadCount(size_t total, size_t cursor, size_t capacity) {
        return cursor >= total ? 0 : std::min(total - cursor, capacity);
    }
}
