// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common.h>
#include <filesystem>
namespace skyline::mods {
struct IpsWrite { u64 offset; std::vector<u8> value; };
struct IpsPatch { std::filesystem::path path; bool ips32{}; std::vector<IpsWrite> writes; };
inline std::vector<IpsPatch> CollectIpsPatches(const std::string &, u64, const std::array<u64, 4> &) { return {}; }
}
