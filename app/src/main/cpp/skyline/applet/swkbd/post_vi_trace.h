// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <common.h>

namespace skyline::applet::swkbd::trace {
    inline thread_local bool postViSvcTraceArmed{};
    inline thread_local u32 postViWaitSequence{};

    inline void ArmPostViSvcTrace() {
        postViSvcTraceArmed = true;
        postViWaitSequence = 0;
    }

    inline bool PostViSvcTraceActive() {
        return postViSvcTraceArmed;
    }

    inline u32 NextPostViWaitSequence() {
        return ++postViWaitSequence;
    }

    inline void FinishPostViSvcTrace() {
        postViSvcTraceArmed = false;
    }
}
