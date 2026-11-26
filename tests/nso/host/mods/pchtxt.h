// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <mods/ips.h>
namespace skyline::mods {
struct PchtxtPatch { std::filesystem::path path; std::vector<IpsWrite> writes; };
inline std::vector<PchtxtPatch> CollectPchtxtPatches(const std::string &, u64, const std::array<u64, 4> &) { return {}; }
}
