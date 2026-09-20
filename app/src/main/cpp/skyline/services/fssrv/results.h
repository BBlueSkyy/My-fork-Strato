// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>

namespace skyline::service::fssrv::result {
    constexpr Result PathDoesNotExist(2, 1);
    constexpr Result PathAlreadyExists(2, 2);
    constexpr Result NoRomFsAvailable(2, 1001);
    constexpr Result EntityNotFound(2, 1002);
    constexpr Result NotImplemented(2, 3001);
    constexpr Result AlreadyExists(2, 3003);
    constexpr Result OutOfRange(2, 3005);
    constexpr Result UnexpectedFailure(2, 5000);
    constexpr Result InvalidArgument(2, 6001);
    constexpr Result InvalidPath(2, 6002);
    constexpr Result TooLongPath(2, 6003);
    constexpr Result InvalidOffset(2, 6061);
    constexpr Result InvalidSize(2, 6062);
    constexpr Result InvalidOpenMode(2, 6072);
    constexpr Result FileExtensionWithoutOpenModeAllowAppend(2, 6201);
    constexpr Result ReadNotPermitted(2, 6202);
    constexpr Result WriteNotPermitted(2, 6203);
    constexpr Result PermissionDenied(2, 6400);
}
