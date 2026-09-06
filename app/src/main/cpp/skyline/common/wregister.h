// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <fmt/format.h>
#include <common/base.h>

namespace skyline {
    struct WRegister {
        u32 lower;
        u32 upper;

        constexpr operator u32() const {
            return lower;
        }

        void operator=(u32 value) {
            lower = value;
            upper = 0;
        }
    };
}

template<>
struct fmt::formatter<skyline::WRegister> : fmt::formatter<skyline::u32> {
    template<typename FormatContext>
    auto format(const skyline::WRegister &reg, FormatContext &ctx) const {
        return fmt::formatter<skyline::u32>::format(static_cast<skyline::u32>(reg), ctx);
    }
};
