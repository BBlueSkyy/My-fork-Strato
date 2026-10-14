// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
#pragma once
#include "guest.h"
#include "instructions.h"

namespace skyline::nce {
    constexpr size_t TrampolineSize{20}; // Size of the main SVC/hook trampoline in u32 instructions

    /**
     * @brief Writes a trampoline to the given target address that saves the current context and calls the given function
     */
    inline u32 *WriteTrampoline(u32 *code, u64 target) {
        /* Hook Trampoline */
        /* Store LR in 16B of pre-allocated stack */
        *code++ = 0xF90007FE; // STR LR, [SP, #8]

        /* Replace Skyline TLS with host TLS */
        *code++ = 0xD53BD041; // MRS X1, TPIDR_EL0
        *code++ = 0xF9415022; // LDR X2, [X1, #0x2A0] (ThreadContext::hostTpidrEl0)
        *code++ = 0xD51BD042; // MSR TPIDR_EL0, X2

        /* Replace guest stack with host stack */
        *code++ = 0x910003E2; // MOV X2, SP
        *code++ = 0xF9415423; // LDR X3, [X1, #0x2A8] (ThreadContext::hostSp)
        *code++ = 0x9100007F; // MOV SP, X3

        /* Store Skyline TLS + guest SP on stack */
        *code++ = 0xA9BF0BE1; // STP X1, X2, [SP, #-16]!

        /* Jump to SvcHandler */
        for (const auto &mov : instructions::MoveRegister(registers::X5, target)) {
            if (mov)
                *code++ = mov;
            else
                *code++ = 0xD503201F; // NOP
        }
        // Additional arguments for SvcHandler; HookHandler ignores them.
        *code++ = 0xF94007E2; // LDR X2, [SP, #8] (saved guest SP)
        *code++ = 0xAA1D03E3; // MOV X3, X29 (guest FP)
        *code++ = 0xD63F00A0; // BLR X5 (X4 retains the original SVC PC)

        /* Restore Skyline TLS + guest SP */
        *code++ = 0xA8C10BE1; // LDP X1, X2, [SP], #16
        *code++ = 0xD51BD041; // MSR TPIDR_EL0, X1
        *code++ = 0x9100005F; // MOV SP, X2

        /* Restore LR and Return */
        *code++ = 0xF94007FE; // LDR LR, [SP, #8]
        *code++ = 0xD65F03C0; // RET

        return code;
    }

}
