#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace skyline {
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    using i32 = std::int32_t;

    template<typename T>
    using span = std::span<T>;

    class exception : public std::runtime_error {
      public:
        using std::runtime_error::runtime_error;
    };
}
