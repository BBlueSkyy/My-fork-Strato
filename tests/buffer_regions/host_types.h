// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
namespace vk {using DeviceSize=uint64_t;}
namespace skyline {
    using u8 = uint8_t;
    using i64 = int64_t;
    namespace constant {
        inline constexpr size_t PageSize = 4096;
    }
    template <class T> struct span : std::span<T> {
        using std::span<T>::span;
        bool valid() const {
            return this->data() != nullptr;
        }
        span(T &value) : std::span<T>{&value, 1} {}
        template <class R> span(R &range) : std::span<T>{range.data(), range.size()} {}
        operator std::vector<T>() const { return {this->begin(), this->end()}; }
    };
    namespace util {
        inline i64 GetTimeNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        template <class T> T *AlignDown(T *p, size_t a) {
            return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(p) & ~(a - 1));
        }
        template <class T> T *AlignUp(T *p, size_t a) {
            return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(p) + a - 1) & ~(a - 1));
        }
    } // namespace util
} // namespace skyline
#define TRACE_EVENT(...)
