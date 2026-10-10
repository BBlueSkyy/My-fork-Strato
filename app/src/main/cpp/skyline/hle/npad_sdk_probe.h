// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <optional>
#include "symbol_hooks.h"

namespace skyline::hle {
    std::optional<HookType> MakeNpadSdkProbe(std::string_view prettyName);
}
