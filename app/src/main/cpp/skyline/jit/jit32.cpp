// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include "jit32.h"
#include "halt_reason.h"
#include <common/trap_manager.h>
#include <common/signal.h>
#include <kernel/scheduler.h>
#include <kernel/types/KThread.h>
#include <kernel/types/KProcess.h>

namespace skyline::jit {
    static std::array<JitCore32, CoreCount> MakeJitCores(const DeviceState &state, Dynarmic::ExclusiveMonitor &monitor) {
        // AArch32 guest code executes inside Dynarmic as ordinary host code, so scheduler
        // signals must halt the JIT rather than going through the NCE guest signal path.
        signal::SetHostSignalHandler({kernel::Scheduler::YieldSignal,
                                      kernel::Scheduler::PreemptionSignal,
                                      SIGINT,
                                      SIGILL,
                                      SIGTRAP,
                                      SIGBUS,
                                      SIGFPE,
                                      SIGSEGV},
                                     Jit32::SignalHandler,
                                     false);

        return {JitCore32(state, monitor, 0),
                JitCore32(state, monitor, 1),
                JitCore32(state, monitor, 2),
                JitCore32(state, monitor, 3)};
    }

    Jit32::Jit32(DeviceState &state)
        : state{state},
          monitor{CoreCount},
          cores{MakeJitCores(state, monitor)} {}

    JitCore32 &Jit32::GetCore(u32 coreId) {
        return cores[coreId];
    }

    void Jit32::SignalHandler(int signal, siginfo *info, ucontext *ctx) {
        auto thread{DeviceState::thread};

        if (signal == kernel::Scheduler::YieldSignal || signal == kernel::Scheduler::PreemptionSignal) {
            kernel::Scheduler::YieldPending = true;
            if (thread && thread->jit)
                thread->jit->HaltExecution(HaltReason::Preempted);
            return;
        }

        if (signal == SIGSEGV) {
            // Handle accesses to regions tracked by TrapManager before considering this a JIT crash.
            if (TrapManager::TrapHandler(reinterpret_cast<u8 *>(info->si_addr), true))
                return;
        }

        const bool isGuest{thread && thread->jit != nullptr};
        if (!isGuest) {
            signal::ExceptionalSignalHandler(signal, info, ctx);
            return;
        }

        auto &mctx{ctx->uc_mcontext};
        if (signal != SIGINT) {
            std::string cpuContext;
            if (mctx.fault_address)
                cpuContext += fmt::format("\n  Fault Address: 0x{:X}", mctx.fault_address);
            if (mctx.sp)
                cpuContext += fmt::format("\n  Host Stack Pointer: 0x{:X}", mctx.sp);

            LOGE("AArch32 HOS-{} crashed due to host signal: {}{}", thread->id, strsignal(signal), cpuContext);

            if (thread->id) {
                signal::BlockSignal({SIGINT});
                thread->GetProcess()->Kill(false);
            }
        }

        // Return from the JIT host frame to KThread's setjmp recovery path.
        mctx.pc = reinterpret_cast<u64>(&std::longjmp);
        mctx.regs[0] = reinterpret_cast<u64>(thread->originalCtx);
        mctx.regs[1] = true;
    }
}
