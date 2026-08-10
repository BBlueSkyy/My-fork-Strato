// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>
#include "config.h"

struct AVFrame;

namespace skyline::soc::host1x::vic {
    /**
     * @brief Writes the supplied decoded frame to the guest NV12 (Y8__V8U8_N420) output surface described by the config, honouring pitch or block-linear layout
     * @param frame The decoded frame to convert, when null a solid test pattern is written instead so output surface plumbing stays observable
     */
    void WriteNv12Surface(const DeviceState &state, const ConfigStruct &config, const PlaneOffsets &outputSurface, AVFrame *frame);

    /**
     * @brief Converts the supplied decoded frame to RGBA and writes it to the guest output surface described by the config, honouring pitch or block-linear layout
     * @param frame The decoded frame to convert, when null a solid test pattern is written instead
     */
    void WriteRgbaSurface(const DeviceState &state, const ConfigStruct &config, const PlaneOffsets &outputSurface, AVFrame *frame);
}
