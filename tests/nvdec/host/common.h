#pragma once
#include <algorithm>
#include <iostream>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#define LOGD(...) skyline::HostLog(__VA_ARGS__)
#define LOGI(...) ((void)0)
#define LOGW(message, ...) (std::cerr << message << "\n")
#define LOGE(...) ((void)0)
namespace skyline {
template<class... Args> void HostLog(const char *,const Args &...) {}
using u128=__uint128_t; using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
using i8=int8_t; using i16=int16_t; using i32=int32_t; using i64=int64_t;
template<class T> using span=std::span<T>;
namespace soc { struct HostSoc; }
struct DeviceState { std::shared_ptr<soc::HostSoc> soc; };
namespace util {
template<class T,class U> constexpr bool IsAligned(T v,U a) { return v%a==0; }
template<class T,class U> constexpr auto AlignDown(T v,U a) { return v/a*a; }
template<class T,class U> constexpr auto AlignUp(T v,U a) { return (v+a-1)/a*a; }
template<class T,class U> constexpr auto DivideCeil(T v,U d) { return (v+d-1)/d; }
}
}
