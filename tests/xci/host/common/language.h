// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
namespace skyline::language {
enum class ApplicationLanguage : u32;
inline std::string ToString(ApplicationLanguage) { return "AmericanEnglish"; }
}
