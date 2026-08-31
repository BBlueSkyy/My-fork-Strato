// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common.h>
#include <common/wregister.h>

namespace skyline {
    struct DeviceState;
    namespace constant {
        constexpr u64 SkyTlsMagic{util::MakeMagic<u64>("SKYTLS")};
    }
    namespace nce {
        /**
         * @brief A complete, persistent snapshot of an AArch64 guest thread
         * @note The layout intentionally matches the 64-bit Horizon ThreadContext returned by GetThreadContext3
         */
        struct alignas(16) GuestCpuContext {
            std::array<u64, 29> gpr;
            u64 fp;
            u64 lr;
            u64 sp;
            u64 pc;
            u32 pstate;
            u32 padding;
            std::array<u128, 32> vreg;
            u32 fpcr;
            u32 fpsr;
            u64 tpidr;
        };
        static_assert(alignof(GuestCpuContext) == 0x10);
        static_assert(offsetof(GuestCpuContext, gpr) == 0x0);
        static_assert(offsetof(GuestCpuContext, fp) == 0xE8);
        static_assert(offsetof(GuestCpuContext, lr) == 0xF0);
        static_assert(offsetof(GuestCpuContext, sp) == 0xF8);
        static_assert(offsetof(GuestCpuContext, pc) == 0x100);
        static_assert(offsetof(GuestCpuContext, pstate) == 0x108);
        static_assert(offsetof(GuestCpuContext, vreg) == 0x110);
        static_assert(offsetof(GuestCpuContext, fpcr) == 0x310);
        static_assert(offsetof(GuestCpuContext, fpsr) == 0x314);
        static_assert(offsetof(GuestCpuContext, tpidr) == 0x318);
        static_assert(sizeof(GuestCpuContext) == 0x320);

        /**
         * @brief The state of callee-saved general purpose registers in the guest
         * @note Read about ARMv8 registers here: https://developer.arm.com/architectures/learn-the-architecture/armv8-a-instruction-set-architecture/registers-in-aarch64-general-purpose-registers
         * @note Read about ARMv8 ABI here: https://github.com/ARM-software/abi-aa/blob/2f1ac56a7d79f3e753e6ca88d4d3e083c31d6f64/aapcs64/aapcs64.rst#machine-registers
         */
        union GpRegisters {
            std::array<u64, 19> regs;
            struct {
                u64 x0;
                u64 x1;
                u64 x2;
                u64 x3;
                u64 x4;
                u64 x5;
                u64 x6;
                u64 x7;
                u64 x8;
                u64 x9;
                u64 x10;
                u64 x11;
                u64 x12;
                u64 x13;
                u64 x14;
                u64 x15;
                u64 x16;
                u64 x17;
                u64 x18;
            };
            struct {
                WRegister w0;
                WRegister w1;
                WRegister w2;
                WRegister w3;
                WRegister w4;
                WRegister w5;
                WRegister w6;
                WRegister w7;
                WRegister w8;
                WRegister w9;
                WRegister w10;
                WRegister w11;
                WRegister w12;
                WRegister w13;
                WRegister w14;
                WRegister w15;
                WRegister w16;
                WRegister w17;
                WRegister w18;
            };
        };

        /**
         * @brief The state of all floating point (and SIMD) registers in the guest
         * @note FPSR/FPCR are 64-bit system registers but only the lower 32-bits are used
         * @note Read about ARMv8 ABI here: https://github.com/ARM-software/abi-aa/blob/2f1ac56a7d79f3e753e6ca88d4d3e083c31d6f64/aapcs64/aapcs64.rst#612simd-and-floating-point-registers
         */
        union alignas(16) FpRegisters {
            std::array<u128, 32> regs;
            u32 fpsr;
            u32 fpcr;
        };

        /**
         * @brief A per-thread context for guest threads
         * @note It's stored in TPIDR_EL0 while running the guest
         */
        struct ThreadContext {
            GpRegisters gpr;
            FpRegisters fpr;
            u8 *hostTpidrEl0; //!< Host TLS TPIDR_EL0, this must be swapped to prior to calling any CXX functions
            u8 *hostSp; //!< Host Stack Pointer, same as above
            u8 *tpidrroEl0; //!< Emulated HOS TPIDRRO_EL0
            u8 *tpidrEl0; //!< Emulated HOS TPIDR_EL0
            u32 nzcv;
            const DeviceState *state;
            u64 magic{constant::SkyTlsMagic};
            GuestCpuContext *guestCpuContext; //!< Unpublished full-context capture buffer owned by the corresponding KThread
        };

        static_assert(offsetof(ThreadContext, gpr) == 0x0);
        static_assert(offsetof(ThreadContext, fpr) == 0xA0);
        static_assert(offsetof(ThreadContext, hostTpidrEl0) == 0x2A0);
        static_assert(offsetof(ThreadContext, hostSp) == 0x2A8);
        static_assert(offsetof(ThreadContext, tpidrroEl0) == 0x2B0);
        static_assert(offsetof(ThreadContext, tpidrEl0) == 0x2B8);
        static_assert(offsetof(ThreadContext, nzcv) == 0x2C0);
        static_assert(offsetof(ThreadContext, state) == 0x2C8);
        static_assert(offsetof(ThreadContext, magic) == 0x2D0);
        static_assert(offsetof(ThreadContext, guestCpuContext) == 0x2D8);
        static_assert(sizeof(ThreadContext) == 0x2E0);

        namespace guest {
            constexpr size_t SaveCtxSize{83}; //!< The size of the SaveCtx function in 32-bit ARMv8 instructions
            constexpr size_t LoadCtxSize{36}; //!< The size of the LoadCtx function in 32-bit ARMv8 instructions

            /**
             * @brief Saves the context from CPU registers into TLS
             * @note Assumes that 8B is reserved at an offset of 8B from SP
             */
            extern "C" void SaveCtx(void);

            /**
             * @brief Loads the context from TLS into CPU registers
             * @note Assumes that 8B is reserved at an offset of 8B from SP
             */
            extern "C" void LoadCtx(void);
        }
    }
}
