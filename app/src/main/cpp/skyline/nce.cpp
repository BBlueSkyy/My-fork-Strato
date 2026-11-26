// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <fstream>
#include <cxxabi.h>
#include <linux/elf.h>
#include "common/signal.h"
#include "common/trace.h"
#include "os.h"
#include "jvm.h"
#include "kernel/types/KProcess.h"
#include "kernel/svc.h"
#include "nce/guest.h"
#include "nce/instructions.h"
#include "nce/acquire_lock_patch.h"
#include "nce.h"
#include <common/fault_breadcrumb.h>

namespace skyline::nce {
    NCE::ExitException::ExitException(bool killAllThreads) : killAllThreads(killAllThreads) {}

    const char *NCE::ExitException::what() const noexcept {
        return killAllThreads ? "ExitProcess" : "ExitThread";
    }


#ifdef __ANDROID__
    // RING-DIAG (temporary, one-shot at two ring-full milestones): ring/consumer/pending-lock state and what every guest thread is doing in the kernel.
    static void RingDiagDump(const DeviceState &state, u64 ringVa, u64 count) {
        constexpr u64 TextBase{0x800009000ULL}; // main.nso .text (module offsets in the static analysis are relative to this)
        auto &mem{state.process->memory};
        const auto p8{[&](u64 va) { return mem.TranslateVirtualPointer<u8 *>(va); }};
        const auto r32{[&](u64 va) { return *reinterpret_cast<u32 *>(p8(va)); }};
        const auto r64{[&](u64 va) { return *reinterpret_cast<u64 *>(p8(va)); }};

        const u32 w{r32(ringVa + 0x0)}, cachedR{r32(ringVa + 0x40)}, wake{r32(ringVa + 0x50)}, reclaim{r32(ringVa + 0x80)};
        const u32 rd{r32(ringVa + 0xC0)}, cachedW{r32(ringVa + 0xC4)}, size{r32(ringVa + 0x138)}, mask{r32(ringVa + 0x13C)}, thresh{r32(ringVa + 0x140)};
        const u64 lastEntry{r64(ringVa + 0x48)}, curEntry{r64(ringVa + 0xC8)}, ctx{r64(ringVa + 0x100)}, base{r64(ringVa + 0x130)};
        const u64 consumerEvent{r64(ringVa + 0x120)}, syncEvent{r64(ringVa + 0x128)};
        LOGW("SCHED-DIAG ringdump#{} ring=0x{:X} W=0x{:X} cachedR=0x{:X} R=0x{:X} Rd=0x{:X} cachedW=0x{:X} size=0x{:X} mask=0x{:X} wake={}/{} base=0x{:X} ctx=0x{:X} lastEntry=0x{:X} curEntry=0x{:X} waitFlag={} initFlag={} evCons=0x{:X} evSync=0x{:X}",
             count, ringVa, w, cachedR, reclaim, rd, cachedW, size, mask, wake, thresh, base, ctx, lastEntry, curEntry,
             *p8(ringVa + 0x119), *p8(ringVa + 0x118), consumerEvent, syncEvent);
        if (!base || !size || size > 0x10000000 || (size & (size - 1)) || !ctx)
            return;

        struct Entry { u32 off, size, state; u64 handler, obj; u32 flags; };
        const auto entry{[&](u32 off) -> Entry {
            const u64 va{base + off};
            return {off, r32(va), r32(va + 4), r64(va + 8), r64(va + 0x10), r32(va + 0x18)};
        }};
        const auto print{[&](const char *tag, const Entry &e) {
            LOGW("SCHED-DIAG ringdump#{} {} off=0x{:X} size=0x{:X} state={} handler=text+0x{:X} obj=0x{:X} flags=0x{:X}",
                 count, tag, e.off, e.size, e.state, e.handler - TextBase, e.obj, e.flags);
        }};
        const auto valid{[&](const Entry &e) { return e.size >= 0x40 && !(e.size & 0x3F) && e.size <= size; }};

        // The oldest unreclaimed entry, then the following ones (iteration structure of the command stream).
        u32 off{reclaim};
        for (unsigned i{}; i < 10; ++i) {
            auto e{entry(off)};
            print(i == 0 ? "R+0(oldest-unreclaimed)" : "R+n", e);
            if (!valid(e) || off == w) break;
            off = (off + e.size) & mask;
        }
        // Everything between R and W: state histogram + every entry that is not finished (state 1 = unconsumed, 2 = pending lock).
        std::array<u32, 8> hist{};
        unsigned shown{};
        off = reclaim;
        for (u64 walked{}; walked < 200000; ++walked) {
            auto e{entry(off)};
            if (!valid(e)) { LOGW("SCHED-DIAG ringdump#{} walk-stop invalid entry at 0x{:X} size=0x{:X} state={}", count, off, e.size, e.state); break; }
            ++hist[e.state & 7];
            if ((e.state == 1 || e.state == 2) && shown++ < 24)
                print(e.state == 2 ? "PENDING(state2)" : "UNCONSUMED(state1)", e);
            off = (off + e.size) & mask;
            if (off == w) break;
        }
        LOGW("SCHED-DIAG ringdump#{} hist state0={} state1={} state2={} state3={} state4={}", count, hist[0], hist[1], hist[2], hist[3], hist[4]);

        // Entry at the consumer read cursor, the entry the consumer last started, and the lap start up to W.
        print("consumer-read-cursor", entry(rd));
        if (curEntry >= base && curEntry < base + size)
            print("consumer-current", entry(static_cast<u32>(curEntry - base)));
        off = 0;
        for (unsigned i{}; i < 8 && off != w; ++i) {
            auto e{entry(off)};
            print("lap-start", e);
            if (!valid(e)) break;
            off += e.size;
        }

        // Device-context vectors: pending staged locks (A), locked objects (B), producer-side direct-lock records.
        const u64 aBegin{r64(ctx + 0x4180)}, aEnd{r64(ctx + 0x4188)}, bBegin{r64(ctx + 0x41B0)}, bEnd{r64(ctx + 0x41B8)}, cBegin{r64(ctx + 0x4198)}, cEnd{r64(ctx + 0x41A0)};
        LOGW("SCHED-DIAG ringdump#{} vectors pendingLocks={} lockedObjects={} producerRecords={}", count, (aEnd - aBegin) / 8, (bEnd - bBegin) / 8, (cEnd - cBegin) / 24);
        for (u64 a{aBegin}, i{}; a < aEnd && i < 16; a += 8, ++i) {
            const u64 e{r64(a)};
            if (e < base || e >= base + size) { LOGW("SCHED-DIAG ringdump#{} pendingLock[{}] entry=0x{:X} outside ring", count, i, e); continue; }
            LOGW("SCHED-DIAG ringdump#{} pendingLock[{}] entry=0x{:X} ringOff=0x{:X} obj=0x{:X} flags=0x{:X}", count, i, e, e - base, r64(e + 0x10), r32(e + 0x18));
        }
        for (u64 a{bBegin}, i{}; a < bEnd && i < 16; a += 8, ++i)
            LOGW("SCHED-DIAG ringdump#{} lockedObject[{}]=0x{:X}", count, i, r64(a));
        for (u64 a{cBegin}, i{}; a < cEnd && i < 16; a += 24, ++i)
            LOGW("SCHED-DIAG ringdump#{} producerRecord[{}] obj=0x{:X} flags=0x{:X} ptr=0x{:X}", count, i, r64(a), r32(a + 8), r64(a + 0x10));

        // What every guest thread is doing in the kernel right now.
        const u64 now{static_cast<u64>(util::GetTimeNs())};
        state.process->DiagForEachThread([&](kernel::type::KThread &threadRef) {
            auto *thread{&threadRef};
            auto &d{thread->diagSvc};
            const auto *ipc{d.ipcName.load(std::memory_order_acquire)};
            const auto *nv{d.nvDevice.load(std::memory_order_acquire)};
            LOGW("SCHED-DIAG ringdump#{} thread T{} running={} killed={} paused={} core={} prio={} phase={} svcCount={} lastSvc=0x{:X} pc=0x{:X} ageMs={} x0=0x{:X} x1=0x{:X} x2=0x{:X} x3=0x{:X} x19=0x{:X} x20=0x{:X} x21=0x{:X} ringX21={} ipc={}/0x{:X} nv={}/fd{}/cmd0x{:X} cvWait={} mutexWait={}",
                 count, thread->id, thread->running, thread->killed.load(), thread->isPaused, static_cast<unsigned>(thread->coreId), static_cast<int>(thread->priority.load()),
                 d.phase.load(), d.count.load(), d.id.load(), d.pc.load(), (now - d.enterNs.load()) / 1000000,
                 d.x0.load(), d.x1.load(), d.x2.load(), d.x3.load(), d.x19.load(), d.x20.load(), d.x21.load(),
                 d.x21.load() == ringVa || d.x20.load() == ringVa || d.x19.load() == ringVa,
                 ipc ? ipc : "-", d.ipcCmd.load(), nv ? nv : "-", d.nvFd.load(), d.nvCmd.load(),
                 thread->waitConditionVariable != nullptr, thread->waitMutex != nullptr);
            if (d.phase.load() == 1 && d.id.load() == 0x19 && d.x2.load() <= 8 && d.x1.load()) { // WaitSynchronization: list the waited handles
                const auto *handles{mem.TranslateVirtualPointer<u32 *>(d.x1.load())};
                for (u64 i{}; i < d.x2.load(); ++i) {
                    int type{-1};
                    try {
                        if (const auto object{state.process->GetHandle(handles[i])})
                            type = static_cast<int>(object->objectType);
                    } catch (const std::exception &) {}
                    LOGW("SCHED-DIAG ringdump#{} T{} waitsOn handle=0x{:X} type={}", count, thread->id, handles[i], type);
                }
            }
        });
    }
#endif

    void NCE::SvcHandler(u16 svcId, ThreadContext *ctx, u64, u64 guestReturnAddress, u64 guestCallerReturnAddress, u64 guestX19, u64 guestX20, u64 guestX21) {
        TRACE_EVENT_END("guest");

        const auto &state{*ctx->state};
#ifdef __ANDROID__
        { // RING-DIAG: current kernel activity of this thread (read once by the ring-full dump)
            auto &d{state.thread->diagSvc};
            d.count.fetch_add(1, std::memory_order_relaxed);
            d.id.store(svcId, std::memory_order_relaxed);
            d.pc.store(guestReturnAddress >= sizeof(u32) ? guestReturnAddress - sizeof(u32) : 0, std::memory_order_relaxed);
            d.x0.store(ctx->gpr.x0, std::memory_order_relaxed);
            d.x1.store(ctx->gpr.x1, std::memory_order_relaxed);
            d.x2.store(ctx->gpr.x2, std::memory_order_relaxed);
            d.x3.store(ctx->gpr.x3, std::memory_order_relaxed);
            d.x19.store(guestX19, std::memory_order_relaxed);
            d.x20.store(guestX20, std::memory_order_relaxed);
            d.x21.store(guestX21, std::memory_order_relaxed);
            d.enterNs.store(util::GetTimeNs(), std::memory_order_relaxed);
            d.phase.store(1, std::memory_order_release);
        }
        if (state.thread->startupSamplerStarted.load(std::memory_order_acquire)) {
            state.thread->startupSvcSequence.fetch_add(1, std::memory_order_acq_rel);
            state.thread->startupSvcId.store(svcId, std::memory_order_relaxed);
            state.thread->startupSvcPc.store(guestReturnAddress >= sizeof(u32) ? guestReturnAddress - sizeof(u32) : 0, std::memory_order_relaxed);
            state.thread->startupSvcCaller.store(guestCallerReturnAddress >= sizeof(u32) ? guestCallerReturnAddress - sizeof(u32) : 0, std::memory_order_relaxed);
            state.thread->startupSvcArg0.store(ctx->gpr.x0, std::memory_order_relaxed);
            state.thread->startupSvcArg1.store(ctx->gpr.x1, std::memory_order_relaxed);
            state.thread->startupSvcX19.store(guestX19, std::memory_order_relaxed);
            state.thread->startupSvcX20.store(guestX20, std::memory_order_relaxed);
            state.thread->startupSvcX21.store(guestX21, std::memory_order_relaxed);
            state.thread->startupSvcSequence.fetch_add(1, std::memory_order_release);
        }
#endif
        auto svc{kernel::svc::SvcTable[svcId]};
#ifdef __ANDROID__
        static thread_local u64 ringYieldCount{};
        const u64 guestCallerPc{guestCallerReturnAddress >= sizeof(u32) ? guestCallerReturnAddress - sizeof(u32) : 0};
        const bool ringFullYield{svcId == 0xB &&
                                 static_cast<i64>(ctx->gpr.x0) == -1 &&
                                 guestCallerPc == 0x8006194C0ULL};
        bool sampleRingYield{};
        if (ringFullYield) {
            ++ringYieldCount;
            sampleRingYield = ringYieldCount == 1 ||
                              ringYieldCount == 4096 ||
                              ringYieldCount == 262144 ||
                              ringYieldCount == 1048576 ||
                              ringYieldCount == 4194304;
            if (sampleRingYield) {
                LOGW("SCHED-DIAG ring-full count={} T{} caller=0x{:X} x19=0x{:X} x20=0x{:X} x21=0x{:X}",
                     ringYieldCount, state.thread->id, guestCallerPc, guestX19, guestX20, guestX21);
                state.scheduler->DiagnosticYieldSnapshot(ringYieldCount, "pre");
                if (ringYieldCount == 1048576 || ringYieldCount == 4194304)
                    RingDiagDump(state, guestX20, ringYieldCount);
            }
        }
#endif
        try {
            if (svc) [[likely]] {
                TRACE_EVENT("kernel", perfetto::StaticString{svc.name});
                auto &svcContext{*reinterpret_cast<kernel::svc::SvcContext *>(ctx)};
                (svc.function)(state, svcContext);
#ifdef __ANDROID__
                if (sampleRingYield)
                    state.scheduler->DiagnosticYieldSnapshot(ringYieldCount, "post");
                state.thread->diagSvc.phase.store(2, std::memory_order_release);
#endif
            } else {
                throw exception("Unimplemented SVC 0x{:X}", svcId);
            }

            while (kernel::Scheduler::YieldPending) [[unlikely]] {
                state.scheduler->Rotate();
                kernel::Scheduler::YieldPending = false;
                state.scheduler->WaitSchedule();
            }
#ifdef __ANDROID__
            state.thread->diagSvc.phase.store(0, std::memory_order_release);
#endif
        } catch (const signal::SignalException &e) {
            if (e.signal != SIGINT) {
                LOGENF("{} (SVC: {})\nStack Trace:{}", e.what(), svc.name, state.loader->GetStackTrace(e.frames));

                if (state.thread->id) {
                    signal::BlockSignal({SIGINT});
                    state.process->Kill(false);
                }
            }

            abi::__cxa_end_catch(); // We call this prior to the longjmp to cause the exception object to be destroyed
            std::longjmp(state.thread->originalCtx, true);
        } catch (const ExitException &e) {
            if (e.killAllThreads && state.thread->id) {
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }

            abi::__cxa_end_catch();
            std::longjmp(state.thread->originalCtx, true);
        } catch (const exception &e) {
            LOGENF("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));

            if (state.thread->id) {
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }

            abi::__cxa_end_catch();
            std::longjmp(state.thread->originalCtx, true);
        } catch (const std::exception &e) {
            if (svc)
                LOGENF("{} (SVC: {})\nStack Trace:{}", e.what(), svc.name, state.loader->GetStackTrace());
            else
                LOGENF("{} (SVC: 0x{:X})\nStack Trace:{}", e.what(), svcId, state.loader->GetStackTrace());

            if (state.thread->id) {
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }

            abi::__cxa_end_catch();
            std::longjmp(state.thread->originalCtx, true);
        }

        TRACE_EVENT_BEGIN("guest", "Guest");
    }

    void NCE::HookHandler(HookId hookId, ThreadContext *ctx) {
        const auto &state{*ctx->state};
        auto hookedSymbol{state.nce->hookedSymbols[hookId.index]};
        try {
            std::visit(VariantVisitor{
                [&](const hle::OverrideHook &hook) {
                    TRACE_EVENT("hook", nullptr, [&](perfetto::EventContext ctx) {
                        ctx.event()->set_name(hookedSymbol.prettyName);
                    });
                    hook(state, hookedSymbol);
                },
                [&](const hle::EntryExitHook &hook) {
                    if (!hookId.isExit) {
                        TRACE_EVENT_BEGIN("hook", nullptr, [&](perfetto::EventContext ctx) {
                            ctx.event()->set_name(hookedSymbol.prettyName);
                        });
                        hook.entry(state, hookedSymbol);
                    } else {
                        hook.exit(state, hookedSymbol);
                        TRACE_EVENT_END("hook");
                    }
                },
            }, hookedSymbol.hook);

            while (kernel::Scheduler::YieldPending) [[unlikely]] {
                state.scheduler->Rotate();
                kernel::Scheduler::YieldPending = false;
                state.scheduler->WaitSchedule();
            }
        } catch (const signal::SignalException &e) {
            if (e.signal != SIGINT) {
                LOGENF("{} (Hook: {})\nStack Trace:{}", e.what(), hookedSymbol.prettyName, state.loader->GetStackTrace(e.frames));

                if (state.thread->id) {
                    signal::BlockSignal({SIGINT});
                    state.process->Kill(false);
                }
            }

            abi::__cxa_end_catch();
            std::longjmp(state.thread->originalCtx, true);
        } catch (const exception &e) {
            LOGENF("{}\nStack Trace:{}", e.what(), state.loader->GetStackTrace(e.frames));

            if (state.thread->id) {
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }

            abi::__cxa_end_catch();
            std::longjmp(state.thread->originalCtx, true);
        } catch (const std::exception &e) {
            LOGENF("{} (Hook: {})\nStack Trace:{}", e.what(), hookedSymbol.prettyName, state.loader->GetStackTrace());
        }
    }

    void NCE::SignalHandler(int signal, siginfo *info, ucontext *ctx, void **tls) {
        auto &mctx{ctx->uc_mcontext};
        const auto &state{*reinterpret_cast<ThreadContext *>(*tls)->state};

        if (signal == SIGSEGV)
            // If we get a guest access violation then we want to handle any accesses that may be from a trapped region
            if (TrapManager::TrapHandler(reinterpret_cast<u8 *>(info->si_addr), true, info->si_code == SEGV_ACCERR))
                return;

        if (signal != SIGINT) {
            signal::StackFrame topFrame{.lr = reinterpret_cast<void *>(ctx->uc_mcontext.pc), .next = reinterpret_cast<signal::StackFrame *>(ctx->uc_mcontext.regs[29])};
            std::string trace{state.loader->GetStackTrace(&topFrame)};

            std::string cpuContext;
            if (mctx.fault_address)
                cpuContext += fmt::format("\n  Fault Address: 0x{:X}", mctx.fault_address);
            if (mctx.sp)
                cpuContext += fmt::format("\n  Stack Pointer: 0x{:X}", mctx.sp);
            for (size_t index{}; index < (sizeof(mcontext_t::regs) / sizeof(u64)); index += 2)
                cpuContext += fmt::format("\n  X{:<2}: 0x{:<16X} X{:<2}: 0x{:X}", index, mctx.regs[index], index + 1, mctx.regs[index + 1]);

            LOGE("Thread #{} has crashed due to signal: {}\nStack Trace:{}\nCPU Context:{}", state.thread->id, strsignal(signal), trace, cpuContext);

            if (state.thread->id) {
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }
        }

        mctx.pc = reinterpret_cast<u64>(&std::longjmp);
        mctx.regs[0] = reinterpret_cast<u64>(state.thread->originalCtx);
        mctx.regs[1] = true;

        *tls = nullptr;
    }

    void NCE::HostSignalHandler(int signal, siginfo *info, ucontext *ctx) {
        if (TrapManager::TrapHandler(reinterpret_cast<u8 *>(info->si_addr), true, info->si_code == SEGV_ACCERR))
            return;

        // Not a trap: if this thread was in the middle of a mirrored sparse copy, say which step it died in (written synchronously, the async logger can lose its queue)
        ReportFaultBreadcrumb(info->si_addr);

        bool runningUnderDebugger{[]() {
            static std::ifstream status("/proc/self/status");
            status.seekg(0);

            constexpr std::string_view TracerPidTag{"TracerPid:"};
            for (std::string line; std::getline(status, line);) {
                if (line.starts_with(TracerPidTag)) {
                    line = line.substr(TracerPidTag.size());

                    for (char character : line)
                        if (std::isspace(character))
                            continue;
                        else
                            return character != '0';

                    return false;
                }
            }

            return false;
        }()};

        if (runningUnderDebugger) {
            /* Variables for debugger, these are meant to be read and utilized by the debugger to break in user code with all registers intact */
            void *pc{reinterpret_cast<void *>(ctx->uc_mcontext.pc)}; // Use 'p pc' to get the value of this and 'breakpoint set -t current -a ${value of pc}' to break in user code
            bool shouldReturn{true}; // Set this to false to throw an exception instead of returning

            raise(SIGTRAP); // Notify the debugger if we've got a SIGSEGV as the debugger doesn't catch them by default as they might be hooked

            if (shouldReturn)
                return;
        }

        signal::ExceptionalSignalHandler(signal, info, ctx); // Delegate throwing a host exception to the exceptional signal handler
    }

    void *NceTlsRestorer() {
        ThreadContext *threadCtx;
        asm volatile("MRS %x0, TPIDR_EL0":"=r"(threadCtx));
        if (threadCtx->magic != constant::SkyTlsMagic)
            return nullptr;
        asm volatile("MSR TPIDR_EL0, %x0"::"r"(threadCtx->hostTpidrEl0));
        return threadCtx;
    }

    NCE::NCE(const DeviceState &state) : state(state) {}

    void NCE::InitializeGuestSignals() {
        std::call_once(signalHandlersOnce, [] {
            signal::SetTlsRestorer(&NceTlsRestorer);
            signal::SetGuestSignalHandler({SIGINT, SIGILL, SIGTRAP, SIGBUS, SIGFPE, SIGSEGV}, nce::NCE::SignalHandler);
            signal::SetHostSignalHandler({SIGSEGV}, nce::NCE::HostSignalHandler);
        });
    }

    constexpr size_t TrampolineSize{23}; // Size of the main SVC trampoline function in u32 units

    /**
     * @brief Writes a trampoline to the given target address that saves the current context and calls the given function
     */
    u32 *WriteTrampoline(u32 *code, u64 target) {
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

        // Preserve the original guest LR as the fourth SvcHandler argument.
        // The per-SVC trampoline saves it at its preallocated guest SP slot.
        // SP + 8 is SaveCtx's own BL return and would identify only the patch.
        *code++ = 0xF9400043; // LDR X3, [X2]
        // sched_yield is the only framed function in the YieldThread tail-call
        // chain. Its caller LR sits 24 bytes above the per-SVC trampoline SP.
        *code++ = 0xF9400C44; // LDR X4, [X2, #24]

        // x19-x21 are guest callee-saved registers and remain live across SaveCtx.
        // Pass them directly because ThreadContext only stores x0-x18.
        *code++ = instructions::Mov(registers::X5, registers::X19).raw;
        *code++ = instructions::Mov(registers::X6, registers::X20).raw;
        *code++ = instructions::Mov(registers::X7, registers::X21).raw;

        /* Jump to SvcHandler */
        for (const auto &mov : instructions::MoveRegister(registers::X2, target)) {
            if (mov)
                *code++ = mov;
            else
                *code++ = 0xD503201F; // NOP
        }
        *code++ = 0xD63F0040; // BLR X2

        /* Restore Skyline TLS + guest SP */
        *code++ = 0xA8C10BE1; // LDP X1, X2, [SP], #16
        *code++ = 0xD51BD041; // MSR TPIDR_EL0, X1
        *code++ = 0x9100005F; // MOV SP, X2

        /* Restore LR and Return */
        *code++ = 0xF94007FE; // LDR LR, [SP, #8]
        *code++ = 0xD65F03C0; // RET

        return code;
    }

    constexpr size_t RescaleClockSize{19}; //!< The size of the RescaleClock function in 32-bit ARMv8 instructions

    /**
     * @brief Writes instructions to rescale the host clock to Tegra X1 levels
     * @note Output is on stack with the stack pointer offset 32B from the initial point
     */
    u32 *WriteRescaleClock(u32 *code) {
        /* Reserve 32B of stack */
        /* Save working registers */
        *code++ = 0xD10083FF; // SUB SP, SP, #32
        *code++ = 0xA90107E0; // STP X0, X1, [SP, #16]

        /* Load magic constant */
        *code++ = 0xD28F0860; // MOV X0, #30787
        *code++ = 0xF2AE3680; // MOVK X0, #29108, LSL #16
        *code++ = 0xF2CB5880; // MOVK X0, #23236, LSL #32
        *code++ = 0xF2E14F80; // MOVK X0, #2684, LSL #48

        /* Load clock frequency value */
        for (const auto &mov : instructions::MoveRegister(registers::X1, util::ClockFrequency)) {
            if (mov)
                *code++ = mov;
            else
                *code++ = 0xD503201F; // NOP
        }

        /* Multiply clock frequency by magic constant */
        *code++ = 0xD345FC21; // LSR X1, X1, #5
        *code++ = 0x9BC07C21; // UMULH X1, X1, X0
        *code++ = 0xD347FC21; // LSR X1, X1, #7

        /* Load counter value */
        *code++ = 0xD53BE040; // MRS X0, CNTVCT_EL0

        /* Rescale counter value */
        *code++ = 0x9AC10801; // UDIV X1, X0, X1
        *code++ = 0x8B010421; // ADD X1, X1, X1, LSL #1
        *code++ = 0xD37AE420; // LSL X0, X1, #6

        /* Store result */
        /* Restore registers */
        *code++ = 0xF90003E0; // STR X0, [SP, #0]
        *code++ = 0xA94107E0; // LDP X0, X1, [SP, #16]

        return code;
    }

    constexpr u32 TpidrEl0{0x5E82};         // ID of TPIDR_EL0 in MRS
    constexpr u32 TpidrroEl0{0x5E83};       // ID of TPIDRRO_EL0 in MRS
    constexpr u32 CntfrqEl0{0x5F00};        // ID of CNTFRQ_EL0 in MRS
    constexpr u32 CntpctEl0{0x5F01};        // ID of CNTPCT_EL0 in MRS
    constexpr u32 CntvctEl0{0x5F02};        // ID of CNTVCT_EL0 in MRS
    constexpr u32 TegraX1Freq{19200000};    // The clock frequency of the Tegra X1 (19.2 MHz)

    NCE::PatchData NCE::GetPatchData(const std::vector<u8> &text) {
        size_t size{guest::SaveCtxSize + guest::LoadCtxSize + TrampolineSize};
        std::vector<size_t> offsets;

        bool rescaleClock{util::ClockFrequency != TegraX1Freq};

        auto start{reinterpret_cast<const u32 *>(text.data())}, end{reinterpret_cast<const u32 *>(text.data() + text.size())};
        for (const u32 *instruction{start}; instruction < end; instruction++) {
            auto svc{*reinterpret_cast<const instructions::Svc *>(instruction)};
            auto mrs{*reinterpret_cast<const instructions::Mrs *>(instruction)};
            auto msr{*reinterpret_cast<const instructions::Msr *>(instruction)};
            auto instructionOffset{static_cast<size_t>(instruction - start)};

            if (svc.Verify()) {
                size += 7;
                offsets.push_back(instructionOffset);
            } else if (mrs.Verify()) {
                if (mrs.srcReg == TpidrroEl0 || mrs.srcReg == TpidrEl0) {
                    size += ((mrs.destReg != registers::X0) ? 6 : 3);
                    offsets.push_back(instructionOffset);
                } else {
                    if (rescaleClock) {
                        if (mrs.srcReg == CntpctEl0) {
                            size += RescaleClockSize + 3;
                            offsets.push_back(instructionOffset);
                        } else if (mrs.srcReg == CntfrqEl0) {
                            size += 3;
                            offsets.push_back(instructionOffset);
                        }
                    } else if (mrs.srcReg == CntpctEl0) {
                        offsets.push_back(instructionOffset);
                    }
                }
            } else if (msr.Verify() && msr.destReg == TpidrEl0) {
                size += 6;
                offsets.push_back(instructionOffset);
            }
        }
        return {util::AlignUp(size * sizeof(u32), constant::PageSize), offsets};
    }

    void NCE::PatchCode(std::vector<u8> &text, u32 *patch, size_t patchSize, const std::vector<size_t> &offsets, size_t textOffset) {
        u32 *start{patch};
        u32 *end{patch + (patchSize / sizeof(u32))};

        std::memcpy(patch, reinterpret_cast<void *>(&guest::SaveCtx), guest::SaveCtxSize * sizeof(u32));
        patch += guest::SaveCtxSize;

        patch = WriteTrampoline(patch, reinterpret_cast<u64>(&NCE::SvcHandler));

        std::memcpy(patch, reinterpret_cast<void *>(&guest::LoadCtx), guest::LoadCtxSize * sizeof(u32));
        patch += guest::LoadCtxSize;

        bool rescaleClock{util::ClockFrequency != TegraX1Freq};

        for (auto offset : offsets) {
            u32 *instruction{reinterpret_cast<u32 *>(text.data()) + offset};
            auto svc{*reinterpret_cast<instructions::Svc *>(instruction)};
            auto mrs{*reinterpret_cast<instructions::Mrs *>(instruction)};
            auto msr{*reinterpret_cast<instructions::Msr *>(instruction)};
            auto endOffset{[&] { return static_cast<size_t>(end - patch) + (textOffset / sizeof(u32)); }};
            auto startOffset{[&] { return static_cast<size_t>(start - patch); }};

            if (svc.Verify()) {
                /* Per-SVC Trampoline */
                /* Rewrite SVC with B to trampoline */
                *instruction = instructions::B(static_cast<i32>(endOffset() + offset), true).raw;

                /* Save Context */
                *patch++ = 0xF81F0FFE; // STR LR, [SP, #-16]!
                *patch = instructions::BL(static_cast<i32>(startOffset())).raw;
                patch++;

                /* Jump to main SVC trampoline */
                *patch++ = instructions::Movz(registers::W0, static_cast<u16>(svc.value)).raw;
                *patch = instructions::BL(static_cast<i32>(startOffset() + guest::SaveCtxSize)).raw;
                patch++;

                /* Restore Context and Return */
                *patch = instructions::BL(static_cast<i32>(startOffset() + guest::SaveCtxSize + TrampolineSize)).raw;
                patch++;
                *patch++ = 0xF84107FE; // LDR LR, [SP], #16
                *patch = instructions::B(static_cast<i32>(endOffset() + offset + 1)).raw;
                patch++;
            } else if (mrs.Verify()) {
                if (mrs.srcReg == TpidrroEl0 || mrs.srcReg == TpidrEl0) {
                    /* Emulated TLS Register Load */
                    /* Rewrite MRS with B to trampoline */
                    *instruction = instructions::B(static_cast<i32>(endOffset() + offset), true).raw;

                    /* Allocate Scratch Register */
                    if (mrs.destReg != registers::X0)
                        *patch++ = 0xF81F0FE0; // STR X0, [SP, #-16]!

                    /* Retrieve emulated TLS register from ThreadContext */
                    *patch++ = 0xD53BD040; // MRS X0, TPIDR_EL0
                    if (mrs.srcReg == TpidrroEl0)
                        *patch++ = 0xF9415800; // LDR X0, [X0, #0x2B0] (ThreadContext::tpidrroEl0)
                    else
                        *patch++ = 0xF9415C00; // LDR X0, [X0, #0x2B8] (ThreadContext::tpidrEl0)

                    /* Restore Scratch Register and Return */
                    if (mrs.destReg != registers::X0) {
                        *patch++ = instructions::Mov(registers::X(mrs.destReg), registers::X0).raw;
                        *patch++ = 0xF84107E0; // LDR X0, [SP], #16
                    }
                    *patch = instructions::B(static_cast<i32>(endOffset() + offset + 1)).raw;
                    patch++;
                } else {
                    if (rescaleClock) {
                        if (mrs.srcReg == CntpctEl0) {
                            /* Physical Counter Load Emulation (With Rescaling) */
                            /* Rewrite MRS with B to trampoline */
                            *instruction = instructions::B(static_cast<i32>(endOffset() + offset), true).raw;

                            /* Rescale host clock */
                            patch = WriteRescaleClock(patch);

                            /* Load result from stack into destination register */
                            instructions::Ldr ldr(0xF94003E0); // LDR XOUT, [SP]
                            ldr.destReg = mrs.destReg;
                            *patch++ = ldr.raw;

                            /* Free 32B stack allocation by RescaleClock and Return */
                            *patch++ = {0x910083FF}; // ADD SP, SP, #32
                            *patch = instructions::B(static_cast<i32>(endOffset() + offset + 1)).raw;
                            patch++;
                        } else if (mrs.srcReg == CntfrqEl0) {
                            /* Physical Counter Frequency Load Emulation */
                            /* Rewrite MRS with B to trampoline */
                            *instruction = instructions::B(static_cast<i32>(endOffset() + offset), true).raw;

                            /* Write back Tegra X1 Counter Frequency and Return */
                            for (const auto &mov : instructions::MoveRegister(registers::X(mrs.destReg), TegraX1Freq))
                                *patch++ = mov;
                            *patch = instructions::B(static_cast<i32>(endOffset() + offset + 1)).raw;
                            patch++;
                        }
                    } else if (mrs.srcReg == CntpctEl0) {
                        /* Physical Counter Load Emulation (Without Rescaling) */
                        // We just convert CNTPCT_EL0 -> CNTVCT_EL0 as Linux doesn't allow access to the physical counter
                        *instruction = instructions::Mrs(CntvctEl0, registers::X(mrs.destReg)).raw;
                    }
                }
            } else if (msr.Verify() && msr.destReg == TpidrEl0) {
                /* Emulated TLS Register Store */
                /* Rewrite MSR with B to trampoline */
                *instruction = instructions::B(static_cast<i32>(endOffset() + offset), true).raw;

                /* Allocate Scratch Registers */
                bool x0x1{mrs.srcReg != registers::X0 && mrs.srcReg != registers::X1};
                *patch++ = x0x1 ? 0xA9BF07E0 : 0xA9BF0FE2; // STP X(0/2), X(1/3), [SP, #-16]!

                /* Store new TLS value into ThreadContext */
                *patch++ = x0x1 ? 0xD53BD040 : 0xD53BD042; // MRS X(0/2), TPIDR_EL0
                *patch++ = instructions::Mov(x0x1 ? registers::X1 : registers::X3, registers::X(msr.srcReg)).raw;
                *patch++ = x0x1 ? 0xF9015C01 : 0xF9015C43; // STR X(1/3), [X(0/2), #0x4B8] (ThreadContext::tpidrEl0)

                /* Restore Scratch Registers and Return */
                *patch++ = x0x1 ? 0xA8C107E0 : 0xA8C10FE2; // LDP X(0/2), X(1/3), [SP], #16
                *patch = instructions::B(static_cast<i32>(endOffset() + offset + 1)).raw;
                patch++;
            }
        }

    // Correct guest locks which use exclusive retry loops without acquire ordering.
    // See BOTW 1.6.0's job-queue race: two workers can consume one queue item.
    const auto upgradedAcquires{PatchUnfencedLockAcquires(text.data(), text.size())};
    if (upgradedAcquires)
        LOGI("NCE: upgraded {} unfenced exclusive lock acquires", upgradedAcquires);
    }

    size_t NCE::GetHookSectionSize(span<hle::HookedSymbolEntry> entries) {
        if (entries.empty())
            return 0;

        size_t size{guest::SaveCtxSize + guest::LoadCtxSize + TrampolineSize};
        for (const auto &entry : entries) {
            constexpr size_t EmitTrampolineSize{10};
            if (std::holds_alternative<hle::OverrideHook>(entry.hook))
                size += EmitTrampolineSize + 1;
            else if (std::holds_alternative<hle::EntryExitHook>(entry.hook))
                size += 4 + EmitTrampolineSize + 1 + EmitTrampolineSize + 4 + 1;
        }
        return size * sizeof(u32);
    }

    void NCE::WriteHookSection(span<hle::HookedSymbolEntry> entries, span<u32> hookSection) {
        u32 *start{hookSection.data()};
        u32 *end{hookSection.end().base()};
        u32 *hook{start};

        std::memcpy(hook, reinterpret_cast<void *>(&guest::SaveCtx), guest::SaveCtxSize * sizeof(u32));
        hook += guest::SaveCtxSize;

        hook = WriteTrampoline(hook, reinterpret_cast<u64>(&NCE::HookHandler));

        std::memcpy(hook, reinterpret_cast<void *>(&guest::LoadCtx), guest::LoadCtxSize * sizeof(u32));
        hook += guest::LoadCtxSize;

        u64 hookIndex{static_cast<u64>(hookedSymbols.size())};
        hookedSymbols.reserve(entries.size());
        for (const auto &entry : entries) {
            auto startOffset{[&] { return static_cast<size_t>(start - hook); }};
            auto endOffset{[&] { return static_cast<size_t>(end - hook); }};
            auto emitTrampoline{[&](HookId hookId) {
                /* Save Context */
                *hook++ = 0xF81F0FFE; // STR LR, [SP, #-16]!
                *hook = instructions::BL(static_cast<i32>(startOffset())).raw; // BL SaveCtx
                hook++;

                /* Jump to entry trampoline */
                for (const auto &mov : instructions::MoveRegister(registers::X0, hookId.raw))
                    if (mov)
                        *hook++ = mov;
                *hook = instructions::BL(static_cast<i32>(startOffset() + guest::SaveCtxSize)).raw; // BL HookTrampoline
                hook++;

                /* Restore Context */
                *hook = instructions::BL(static_cast<i32>(startOffset() + guest::SaveCtxSize + TrampolineSize)).raw; // BL LoadCtx
                hook++;
                *hook++ = 0xF84107FE; // LDR LR, [SP], #16
            }};

            Elf64_Addr originalOffset{*entry.offset};
            *entry.offset = -(endOffset() * sizeof(u32));

            if (std::holds_alternative<hle::OverrideHook>(entry.hook)) {
                /* Override Hook */
                emitTrampoline(HookId{hookIndex, false});
            } else if (std::holds_alternative<hle::EntryExitHook>(entry.hook)) {
                /* TLS LR Store */
                *hook++ = 0xA9BF07E0; // STP X0, X1, [SP, #-16]!
                *hook++ = 0xD53BD040; // MRS X0, TPIDR_EL0
                *hook++ = 0xF9415401; // LDR X1, [X0, #0x2A8] (ThreadContext::hostSp)
                *hook++ = 0xF81F0C3E; // STR LR, [X1, #-16]!
                *hook++ = 0xF9015401; // STR X1, [X0, #0x2A8] (ThreadContext::hostSp)
                *hook++ = 0xA8C107E0; // LDP X0, X1, [SP], #16

                /* Entry Hook */
                emitTrampoline(HookId{hookIndex, false});

                /* Function Proxy */
                *hook++ = instructions::BL(static_cast<i32>(endOffset() + (originalOffset / sizeof(u32)))).raw;

                /* Exit Hook */
                emitTrampoline(HookId{hookIndex, true});

                /* TLS LR Load */
                *hook++ = 0xA9BF07E0; // STP X0, X1, [SP, #-16]!
                *hook++ = 0xD53BD040; // MRS X0, TPIDR_EL0
                *hook++ = 0xF9415401; // LDR X1, [X0, #0x2A8] (ThreadContext::hostSp)
                *hook++ = 0xF841043E; // LDR LR, [X1], #16
                *hook++ = 0xF9015401; // STR X1, [X0, #0x2A8] (ThreadContext::hostSp)
                *hook++ = 0xA8C107E0; // LDP X0, X1, [SP], #16
            }

            *hook++ = 0xD65F03C0; // RET

            hookedSymbols.emplace_back(entry);
            hookIndex++;
        }
    }
}
