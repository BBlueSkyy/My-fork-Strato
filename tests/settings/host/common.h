// SPDX-License-Identifier: MPL-2.0
// Minimal host platform boundary; tested settings persistence and IPC helpers are production code.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <common/result.h>
#define LOGW(...) ((void)0)
namespace skyline {
template<class T> using span = std::span<T>;
using LanguageCode = u64;
namespace language {
enum class SystemLanguage : u32 { Japanese, AmericanEnglish, French, German, Italian, Spanish,
    Chinese, Korean, Dutch, Portuguese, Russian, Taiwanese, BritishEnglish, CanadianFrench,
    LatinAmericanSpanish, SimplifiedChinese, TraditionalChinese, BrazilianPortuguese };
inline constexpr std::array<LanguageCode, 18> LanguageCodeList{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
inline LanguageCode GetLanguageCode(SystemLanguage value) { return LanguageCodeList.at(static_cast<size_t>(value)); }
}
namespace region { enum class RegionCode : i32 { Auto = -1, Japan, Usa, Europe, Australia, HongKongTaiwanKorea, China }; }
template<class T> struct Setting { T value{}; const T &operator*() { return value; } void operator=(T v) { value = v; } };
struct Settings {
    Setting<bool> isInternetEnabled;
    Setting<language::SystemLanguage> systemLanguage;
    Setting<region::RegionCode> systemRegion;
};
struct HostOS { std::string privateAppFilesPath; };
struct DeviceState { HostOS *os; std::shared_ptr<Settings> settings; };
}
