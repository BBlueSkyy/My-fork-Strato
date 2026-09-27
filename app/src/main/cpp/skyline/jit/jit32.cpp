// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include "jit32.h"
#include <atomic>
#include <common/trap_manager.h>
#include <common/signal.h>
#include <kernel/types/KThread.h>
#include <kernel/types/KProcess.h>
#include <loader/loader.h>
#include <unistd.h>

namespace skyline::jit {
    namespace {
        std::atomic_flag loggedFirstJitFault = ATOMIC_FLAG_INIT;
    }

    static std::array<JitCore32, CoreCount> MakeJitCores(const DeviceState &state, Dynarmic::ExclusiveMonitor &monitor) {
        // Set the signal handler before creating the JIT cores to ensure proper chaining with the Dynarmic handler which is set during construction
        signal::SetHostSignalHandler({SIGINT, SIGILL, SIGTRAP, SIGBUS, SIGFPE, SIGSEGV}, Jit32::SignalHandler);

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
        if (signal == SIGSEGV)
            // Handle any accesses that may be from a trapped region
            if (TrapManager::TrapHandler(reinterpret_cast<u8 *>(info->si_addr), true))
                return;

        auto &mctx{ctx->uc_mcontext};
        auto thread{kernel::this_thread};
        if (!thread) {
            // ART, its compiler and the UI also run in this process. Their
            // signals belong to Android's original handler, not the HOS JIT.
            if (!loggedFirstJitFault.test_and_set())
                LOGE("First JIT32 process signal: signal={} code={} fault=0x{:X} thread={} core=-1 hostPC=0x{:X} guestPC=0x0 lastSVC=0x0 yieldPending=0",
                     signal, info->si_code, reinterpret_cast<uintptr_t>(info->si_addr), gettid(), mctx.pc);
            signal::ForwardOriginalHostSignal(signal, info, ctx);
            return;
        }
        auto *core{thread->jit.load()};
        bool isGuest{core != nullptr}; // Whether the signal happened while running guest code

        if (isGuest) {
            if (signal == SIGINT) {
                // Let Dynarmic return normally, including its execution guard cleanup.
                // Long-jumping out would also abandon the core's ownership mutex.
                core->HaltExecution(HaltReason::Preempted);
                return;
            }
            if (!loggedFirstJitFault.test_and_set())
                LOGE("First JIT32 signal: signal={} thread={} core={} hostPC=0x{:X} guestPC=0x{:X} lastSVC=0x{:X} yieldPending={}",
                     signal, thread->id, thread->coreId, mctx.pc, core->GetPC(), core->GetLastSwi(), kernel::Scheduler::YieldPending.load());

            thread->jitFaultPc = mctx.pc;
            thread->jitFaultAddress = signal == SIGSEGV ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
            thread->jitFaultSignal.store(signal);
            // Return through Dynarmic so it can clear its execution guard and
            // release runMutex before the guest fault is raised on the host.
            core->HaltExecution(HaltReason::Fault);
        } else {
            if (!loggedFirstJitFault.test_and_set())
                LOGE("First JIT32 host signal: signal={} thread={} core={} hostPC=0x{:X} guestPC=0x0 lastSVC=0x0 yieldPending={}",
                     signal, thread->id, thread->coreId, mctx.pc, kernel::Scheduler::YieldPending.load());
            // During SVC/JNI the core is no longer owned by this thread.
            signal::ExceptionalSignalHandler(signal, info, ctx);
        }
    }
}
