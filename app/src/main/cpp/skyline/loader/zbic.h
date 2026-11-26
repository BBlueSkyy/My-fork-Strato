// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

#pragma once

#include <common.h>

namespace skyline::loader::zbic {
    /**
     * @brief Decompresses one ZBIC frame into an already-sized NSO segment buffer.
     *
     * ZBIC is the NSO compression selected by NsoHeader flag bit 7 on HOS 22.0.0+.
     * The caller supplies the decompressed size from the NSO segment header.
     */
    void Decompress(const std::vector<u8> &compressed, std::vector<u8> &output);
}
