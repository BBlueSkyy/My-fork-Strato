// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace skyline {
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    using i8 = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;

    class exception : public std::runtime_error {
      public:
        template<typename... Args>
        exception(std::string_view message, Args &&...) : std::runtime_error(message) {}
    };

    template<typename T, size_t Extent = std::dynamic_extent>
    class span : public std::span<T, Extent> {
      public:
        using std::span<T, Extent>::span;

        constexpr span(const std::span<T, Extent> &other) : std::span<T, Extent>(other) {}

        constexpr std::string_view as_string(bool nullTerminated = false) const {
            const auto end{nullTerminated ? std::find(this->begin(), this->end(), 0) : this->end()};
            return {reinterpret_cast<const char *>(this->data()), static_cast<size_t>(end - this->begin())};
        }
    };

    template<typename T>
    span(T *, size_t) -> span<T>;

    template<typename T, size_t Size>
    span(std::array<T, Size> &) -> span<T, Size>;
}

#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
