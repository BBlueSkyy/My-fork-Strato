// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <common.h>

namespace skyline::applet::swkbd::trace {
    inline thread_local u32 postViSvcBudget{};

    inline void ArmPostViSvcTrace(u32 budget = 24) {
        postViSvcBudget = budget;
    }

    inline bool PostViSvcTraceActive() {
        return postViSvcBudget != 0;
    }

    inline void ConsumePostViSvcTrace() {
        if (postViSvcBudget)
            --postViSvcBudget;
    }
}
