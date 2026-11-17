// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common/base.h>
#include <common/wregister.h>

namespace skyline::kernel::svc {
    /**
     * @brief Register context for SVCs
     * @note This is used to abstract register access for SVCs, allowing for them to be called seamlessly from NCE or JIT
     * @details The register array is a prefix view of NCE's ThreadContext::gpr and must keep the same register order.
     *          Eight registers are required because AArch32 SVC wrappers may consume R6/R7.
     */
    struct SvcContext {
        union {
            std::array<u64, 8> regs;
            struct {
                u64 x0, x1, x2, x3, x4, x5, x6, x7;
            };
            struct {
                WRegister w0, w1, w2, w3, w4, w5, w6, w7;
            };
        };
    };
}
