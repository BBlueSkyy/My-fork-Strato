// SPDX-License-Identifier: MPL-2.0
// Host-only platform shim. Storage, bucket parsing, compression and AES use production code.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
namespace skyline {
namespace loader { class Loader; }
namespace kernel::type { class KProcess; }
struct DeviceState { std::shared_ptr<loader::Loader> updateLoader; };
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using i8 = int8_t; using i16 = int16_t; using i32 = int32_t; using i64 = int64_t;
class exception : public std::runtime_error {
public:
    template<class... T> exception(std::string_view s, T&&...) : std::runtime_error(std::string(s)) {}
};
template<class T, size_t N = std::dynamic_extent> class span : public std::span<T, N> {
public:
    using std::span<T, N>::span;
    span(std::span<T, N> s) : std::span<T, N>(s) {}
    template<class U> span<U> cast() const {
        if (this->size_bytes() % sizeof(U)) throw exception("Invalid cast size");
        return {reinterpret_cast<U*>(this->data()), this->size_bytes() / sizeof(U)};
    }
    template<class U> U &as() const {
        if (this->size_bytes() < sizeof(U)) throw exception("Invalid object size");
        return *reinterpret_cast<U*>(this->data());
    }
    span<T> subspan(size_t offset, size_t count = std::dynamic_extent) const {
        return std::span<T, N>::subspan(offset, count);
    }
    span<T> first(size_t n) const { return subspan(0, n); }
};
template<class T> span(T*, size_t) -> span<T>;
template<class T> span(std::vector<T>&) -> span<T>;
template<class T, size_t N> span(std::array<T, N>&) -> span<T, N>;
namespace util {
template<class T, size_t N> constexpr T MakeMagic(const char (&s)[N]) {
    T v{}; for (size_t i{}; i < N - 1; ++i) v |= T(static_cast<unsigned char>(s[i])) << (8 * i); return v;
}
template<class T> constexpr T SwapEndianness(T v) {
    T r{}; for (size_t i{}; i < sizeof(T); ++i) { r = (r << 8) | (v & 255); v >>= 8; } return r;
}
}
}
