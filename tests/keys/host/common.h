// SPDX-License-Identifier: MPL-2.0
#pragma once

// Reuse the OS-filesystem platform shim; all key parsing and hex conversion
// logic comes from production sources.
#include "../../fssrv/host/common.h"
#include <map>
#include <unordered_map>
#include <hex_conversion.h>

namespace skyline {
    template<typename T>
    span(std::vector<T> &) -> span<T>;
}
