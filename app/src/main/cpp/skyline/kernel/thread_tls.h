// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <cstring>
#include <common/base.h>

namespace skyline::kernel {
    constexpr size_t CurrentThreadHandleTlsOffset{0x110};
    static_assert(CurrentThreadHandleTlsOffset + sizeof(KHandle) <= 0x200);

    // The TLS slot is guest memory translated to a host pointer. The stored value is a guest handle.
    inline void WriteCurrentThreadHandle(u8 *tlsRegion, KHandle handle) {
        std::memcpy(tlsRegion + CurrentThreadHandleTlsOffset, &handle, sizeof(handle));
    }
}
