// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "output_surface.h"

struct AVFrame;

namespace skyline::soc::host1x::nvdec {
    /** @brief Materializes the decoded Y/UV planes without consuming or changing the AVFrame handed to VIC. */
    void WriteDecodedSurface(const DeviceState &state, const OutputSurface &surface, const AVFrame *frame);
}
