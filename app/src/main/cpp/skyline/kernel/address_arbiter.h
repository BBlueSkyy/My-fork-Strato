// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>

namespace skyline::kernel {
    constexpr bool AddressIsLessThan(std::uint32_t actual, std::uint32_t expected) {
        return std::bit_cast<std::int32_t>(actual) < std::bit_cast<std::int32_t>(expected);
    }

    constexpr std::uint32_t ModifyAddressByWaiterCount(std::uint32_t value, std::int32_t count, std::size_t waiters) {
        if (!waiters)
            return value + 1;
        if (count <= 0 || waiters <= static_cast<std::size_t>(count))
            return value - 1;
        return value;
    }
}
