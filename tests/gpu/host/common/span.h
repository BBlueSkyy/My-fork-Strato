// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace skyline {
    using u8 = std::uint8_t;

    template<typename T, size_t Extent = std::dynamic_extent>
    class span : public std::span<T, Extent> {
      public:
        using std::span<T, Extent>::span;

        constexpr bool contains(const span<T, Extent> &other) const {
            return this->begin() <= other.begin() && this->end() >= other.end();
        }
    };
}
