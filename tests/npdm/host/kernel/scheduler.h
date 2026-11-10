// SPDX-License-Identifier: MPL-2.0
// Host-only platform declarations; NPDM and Backing use production code.
#pragma once
#include <bitset>
#include <common.h>
namespace skyline {
namespace constant { constexpr u8 CoreCount{4}; }
namespace kernel {
using CoreMask = std::bitset<constant::CoreCount>;
struct Priority {
    i8 min;
    i8 max;
    constexpr bool Valid(i8 value) const { return (value >= min) && (value <= max); }
};
}
}
