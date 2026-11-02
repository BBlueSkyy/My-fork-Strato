// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>

namespace skyline::soc::host1x::video_diag {
    // Trace initial playback and periodic progress without per-frame INFO spam.
    // Diagnostic-only helper: never changes NVDEC or VIC execution semantics.
    inline bool Sample(u64 sequence) {
        return sequence <= 24 || sequence % 120 == 0;
    }
}
