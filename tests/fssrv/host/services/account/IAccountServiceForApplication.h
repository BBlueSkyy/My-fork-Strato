// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <common.h>

namespace skyline::service::account {
    struct UserId {
        u64 upper;
        u64 lower;

        constexpr bool operator==(const UserId &) const = default;
    };
    static_assert(sizeof(UserId) == 0x10);
}
