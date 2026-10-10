// SPDX-License-Identifier: MPL-2.0
// Minimal utilities for compiling the real context layout without Android/HLE
// dependencies. No context types or offsets are duplicated in this shim.
#pragma once
#include <array>
#include <cstddef>
#include <string_view>
#include <common/base.h>

namespace skyline::util {
    template<typename Type>
    constexpr Type MakeMagic(std::string_view text) {
        Type result{};
        size_t shift{};
        for (auto c : text) {
            result |= static_cast<Type>(c) << shift;
            shift += 8;
        }
        return result;
    }
}
