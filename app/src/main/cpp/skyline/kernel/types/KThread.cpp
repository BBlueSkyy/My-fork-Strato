// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
#include <asm/sigcontext.h>
#include <unistd.h>
#include <common/signal.h>
#include <common/trace.h>
#include <nce.h>
#include <os.h>
#include <kernel/priority_inheritance.h>
#include "KProcess.h"
#include "KThread.h"

namespace skyline::kernel::type {
    KThread::KThread(const DeviceState &state, KHandle handle, KProcess *parent, size_t id, void *entry, u64 argument, void *stackTop, i8 priority, u8 idealCore)
        : handle(handle),
          parent(parent),
          id(id),
          entry(entry),
          entryArgument(argument),
          stackTop(stackTop),
          priority(priority),
          basePriority(priority),
          idealCore(idealCore),
          coreId(idealCore),
          KSyncObject(state, KType::KThread) {
        affinityMask.set(coreId);
    }

    KThread::~KThread() {
        Kill(true);
        if (thread.joinable()) {
            if (thread.get_id() == std::this_thread::get_id())
                thread.detach();
            else
                thread.join();
        }
    }

    void KThread::StartThread() {
        pthread = pthread_self();
        std::array<char, 16> threadName{};
        if (int result{pthread_getname_np(pthread, threadName.data(), threadName.size())})
            LOGW("Failed to get the thread name: {}", strerror(result));

        if (int result{pthread_setname_np(pthread, fmt::format("HOS-{}", id).c_str())})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        ctx.state = &state;
        state.ctx = &ctx;
        state.thread = shared_from_this();

        sigset_t previousSignalMask;
        pthread_sigmask(SIG_SETMASK, nullptr, &previousSignalMask);
        if (setjmp(originalCtx)) { // Returns 1 if it's returning from guest, 0 otherwise
            sigset_t exitSignals;
            sigemptyset(&exitSignals);
            for (int signal : {Scheduler::YieldSignal, Scheduler::PreemptionSignal, SIGINT})
                sigaddset(&exitSignals, signal);
            pthread_sigmask(SIG_BLOCK, &exitSignals, nullptr);
            killed = true;
            {
                std::scoped_lock lock{contextMutex};
                contextCondition.notify_all();
            }
            state.scheduler->RemoveThread();
            parent->RemoveThreadWaiter(state.thread);
            {
                std::scoped_lock lock{KSyncObject::syncObjectMutex};
                for (auto &weakObject : waitObjects)
                    if (auto object{weakObject.lock()})
                        object->syncObjectWaiters.remove(state.thread);
                waitObjects.clear();
                isCancellable = false;
                wakeObject = nullptr;
            }
            {
                std::scoped_lock lock{statusMutex};
                ready = false;
                if (preemptionTimerCreated) {
                    timer_delete(preemptionTimer);
                    preemptionTimerCreated = false;
                }
                isPreempted = false;
            }
            Signal();
            timespec noWait{};
            while (sigtimedwait(&exitSignals, nullptr, &noWait) >= 0) {}
            Scheduler::YieldPending = false;
            state.ctx = nullptr;
            state.thread.reset();
            pthread_sigmask(SIG_SETMASK, &previousSignalMask, nullptr);

            if (threadName[0] != 'H' || threadName[1] != 'O' || threadName[2] != 'S' || threadName[3] != '-') {
                if (int result{pthread_setname_np(pthread, threadName.data())})
                    LOGW("Failed to set the thread name: {}", strerror(result));
                AsyncLogger::UpdateTag();
            }

            {
                std::scoped_lock lock{statusMutex};
                running = false;
                statusCondition.notify_all();
            }
            return;
        }

        try {
            if (!ctx.tpidrroEl0)
                ctx.tpidrroEl0 = parent->AllocateTlsSlot();
            struct sigevent event{
                .sigev_signo = Scheduler::PreemptionSignal,
                .sigev_notify = SIGEV_THREAD_ID,
                .sigev_notify_thread_id = gettid(),
            };
            {
                std::scoped_lock lock{statusMutex};
                if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &preemptionTimer))
                    throw exception("timer_create has failed with '{}'", strerror(errno));
                preemptionTimerCreated = true;
                ready = true;
                statusCondition.notify_all();
            }
            if (killed)
                throw nce::NCE::ExitException(false);
            ctx.gpr.x0 = entryArgument;
            ctx.gpr.x1 = handle;
            ctx.calleeSaved[11] = reinterpret_cast<u64>(entry);
            ctx.sp = reinterpret_cast<u64>(stackTop);
            ctx.pc = reinterpret_cast<u64>(entry);
            CaptureSvcContext();
            state.scheduler->InsertThread(state.thread);
            state.scheduler->WaitSchedule();
            while (Scheduler::YieldPending) {
                Scheduler::YieldPending = false;
                state.scheduler->Rotate();
                state.scheduler->WaitSchedule();
            }

            LeaveContextSnapshot();
            TRACE_EVENT_BEGIN("guest", "Guest");

            asm volatile(
            "MRS X0, TPIDR_EL0\n\t"
            "MSR TPIDR_EL0, %x0\n\t" // Set TLS to ThreadContext
            "STR X0, [%x0, #0x2A0]\n\t" // Write ThreadContext::hostTpidrEl0
            "MOV X0, SP\n\t"
            "STR X0, [%x0, #0x2A8]\n\t" // Write ThreadContext::hostSp
            "MOV SP, %x1\n\t" // Replace SP with guest stack
            "MOV LR, %x2\n\t" // Store entry in Link Register so it's jumped to on return
            "MOV X0, %x3\n\t" // Store the argument in X0
            "MOV X1, %x4\n\t" // Store the thread handle in X1, NCA applications require this
            "MOV X2, XZR\n\t" // Zero out other GP and SIMD registers, not doing this will break applications
            "MOV X3, XZR\n\t"
            "MOV X4, XZR\n\t"
            "MOV X5, XZR\n\t"
            "MOV X6, XZR\n\t"
            "MOV X7, XZR\n\t"
            "MOV X8, XZR\n\t"
            "MOV X9, XZR\n\t"
            "MOV X10, XZR\n\t"
            "MOV X11, XZR\n\t"
            "MOV X12, XZR\n\t"
            "MOV X13, XZR\n\t"
            "MOV X14, XZR\n\t"
            "MOV X15, XZR\n\t"
            "MOV X16, XZR\n\t"
            "MOV X17, XZR\n\t"
            "MOV X18, XZR\n\t"
            "MOV X19, XZR\n\t"
            "MOV X20, XZR\n\t"
            "MOV X21, XZR\n\t"
            "MOV X22, XZR\n\t"
            "MOV X23, XZR\n\t"
            "MOV X24, XZR\n\t"
            "MOV X25, XZR\n\t"
            "MOV X26, XZR\n\t"
            "MOV X27, XZR\n\t"
            "MOV X28, XZR\n\t"
            "MOV X29, XZR\n\t"
            "MSR FPSR, XZR\n\t"
            "MSR FPCR, XZR\n\t"
            "MSR NZCV, XZR\n\t"
            "DUP V0.16B, WZR\n\t"
            "DUP V1.16B, WZR\n\t"
            "DUP V2.16B, WZR\n\t"
            "DUP V3.16B, WZR\n\t"
            "DUP V4.16B, WZR\n\t"
            "DUP V5.16B, WZR\n\t"
            "DUP V6.16B, WZR\n\t"
            "DUP V7.16B, WZR\n\t"
            "DUP V8.16B, WZR\n\t"
            "DUP V9.16B, WZR\n\t"
            "DUP V10.16B, WZR\n\t"
            "DUP V11.16B, WZR\n\t"
            "DUP V12.16B, WZR\n\t"
            "DUP V13.16B, WZR\n\t"
            "DUP V14.16B, WZR\n\t"
            "DUP V15.16B, WZR\n\t"
            "DUP V16.16B, WZR\n\t"
            "DUP V17.16B, WZR\n\t"
            "DUP V18.16B, WZR\n\t"
            "DUP V19.16B, WZR\n\t"
            "DUP V20.16B, WZR\n\t"
            "DUP V21.16B, WZR\n\t"
            "DUP V22.16B, WZR\n\t"
            "DUP V23.16B, WZR\n\t"
            "DUP V24.16B, WZR\n\t"
            "DUP V25.16B, WZR\n\t"
            "DUP V26.16B, WZR\n\t"
            "DUP V27.16B, WZR\n\t"
            "DUP V28.16B, WZR\n\t"
            "DUP V29.16B, WZR\n\t"
            "DUP V30.16B, WZR\n\t"
            "DUP V31.16B, WZR\n\t"
            "RET"
            :
            : "r"(&ctx), "r"(stackTop), "r"(entry), "r"(entryArgument), "r"(handle)
            : "x0", "x1", "lr"
            );

            __builtin_unreachable();
        } catch (const nce::NCE::ExitException &) {
            abi::__cxa_end_catch();
            std::longjmp(originalCtx, true);
        } catch (const std::exception &e) {
            LOGE("{}", e.what());
            if (id) {
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }
            abi::__cxa_end_catch();
            std::longjmp(originalCtx, true);
        } catch (const signal::SignalException &e) {
            if (e.signal != SIGINT) {
                LOGE("{}", e.what());
                if (id) {
                    signal::BlockSignal({SIGINT});
                    state.process->Kill(false);
                }
            }
            abi::__cxa_end_catch();
            std::longjmp(originalCtx, true);
        }
    }

    void KThread::CaptureSvcContext() {
        std::scoped_lock lock{contextMutex};
        contextSnapshot = {};
        std::copy(ctx.gpr.regs.begin(), ctx.gpr.regs.end(), contextSnapshot.gpr.begin());
        std::copy_n(ctx.calleeSaved.begin(), 10, contextSnapshot.gpr.begin() + 19);
        contextSnapshot.fp = ctx.calleeSaved[10];
        contextSnapshot.lr = ctx.calleeSaved[11];
        contextSnapshot.sp = ctx.sp;
        contextSnapshot.pc = ctx.pc;
        contextSnapshot.pstate = ctx.nzcv;
        contextSnapshot.vreg = ctx.fpr.regs;
        contextSnapshot.fpcr = ctx.fpcr;
        contextSnapshot.fpsr = ctx.fpsr;
        contextSnapshot.tpidr = reinterpret_cast<u64>(ctx.tpidrEl0);
        contextCaptureFailed = false;
        contextAvailable = true;
        contextCondition.notify_all();
    }

    void KThread::CaptureSignalContext(const ucontext &signalContext) {
        std::scoped_lock lock{contextMutex};
        contextSnapshot = {};
        const auto &machine{signalContext.uc_mcontext};
        std::copy_n(machine.regs, 29, contextSnapshot.gpr.begin());
        contextSnapshot.fp = machine.regs[29];
        contextSnapshot.lr = machine.regs[30];
        contextSnapshot.sp = machine.sp;
        contextSnapshot.pc = machine.pc;
        contextSnapshot.pstate = machine.pstate & 0xF0000000;
        contextSnapshot.tpidr = reinterpret_cast<u64>(ctx.tpidrEl0);
        bool hasFp{};
        const auto *cursor{reinterpret_cast<const u8 *>(machine.__reserved)};
        const auto *end{cursor + sizeof(machine.__reserved)};
        while (static_cast<size_t>(end - cursor) >= sizeof(_aarch64_ctx)) {
            const auto *header{reinterpret_cast<const _aarch64_ctx *>(cursor)};
            if (header->size < sizeof(_aarch64_ctx) || header->size > static_cast<size_t>(end - cursor))
                break;
            if (header->magic == FPSIMD_MAGIC && header->size >= sizeof(fpsimd_context)) {
                const auto *fp{reinterpret_cast<const fpsimd_context *>(cursor)};
                std::memcpy(contextSnapshot.vreg.data(), fp->vregs, sizeof(contextSnapshot.vreg));
                contextSnapshot.fpcr = fp->fpcr;
                contextSnapshot.fpsr = fp->fpsr;
                hasFp = true;
                break;
            }
            cursor += header->size;
        }
        contextCaptureFailed = !hasFp;
        contextAvailable = hasFp;
        contextCondition.notify_all();
    }

    void KThread::LeaveContextSnapshot() {
        std::unique_lock lock{contextMutex};
        while (isPaused && !killed) {
            lock.unlock();
            state.scheduler->WaitSchedule();
            lock.lock();
        }
        contextAvailable = false;
    }

    bool KThread::Start(bool self) {
        {
            std::unique_lock lock{statusMutex};
            if (started || killed)
                return false;
            started = true;
            running = true;
            try {
                if (!self)
                    thread = std::thread(&KThread::StartThread, this);
            } catch (...) {
                started = false;
                running = false;
                throw;
            }
        }
        if (self)
            StartThread();
        return true;
    }

    void KThread::Kill(bool join) {
        std::unique_lock lock{statusMutex};
        if (!killed.exchange(true) && running) {
            statusCondition.wait(lock, [this] { return ready || !running; });
            if (ready && running)
                pthread_kill(pthread, SIGINT);
        }
        scheduleCondition.notify();
        {
            std::scoped_lock contextLock{contextMutex};
            contextCondition.notify_all();
        }
        if (join && state.thread.get() != this)
            statusCondition.wait(lock, [this] { return !running; });
    }

    bool KThread::SendSignal(int signal) {
        std::scoped_lock lock{statusMutex};
        return ready && running && !killed && pthread_kill(pthread, signal) == 0;
    }

    void KThread::ArmPreemptionTimer(std::chrono::nanoseconds timeToFire) {
        std::scoped_lock lock{statusMutex};
        if (!ready || !running || killed || !preemptionTimerCreated)
            return;
        auto ns{timeToFire.count()};
        struct itimerspec spec{.it_value = {
            .tv_sec = static_cast<time_t>(ns / 1000000000),
            .tv_nsec = static_cast<long>(ns % 1000000000),
        }};
        if (timer_settime(preemptionTimer, 0, &spec, nullptr))
            throw exception("timer_settime has failed with '{}'", strerror(errno));
        isPreempted = ns != 0;
    }

    void KThread::DisarmPreemptionTimer() {
        std::scoped_lock lock{statusMutex};
        if (preemptionTimerCreated) {
            struct itimerspec spec{};
            if (timer_settime(preemptionTimer, 0, &spec, nullptr))
                throw exception("timer_settime has failed with '{}'", strerror(errno));
        }
        isPreempted = false;
    }

    void KThread::UpdatePriorityInheritance() {
        std::scoped_lock lock{parent->synchronizationMutex};
        RecomputePriorityInheritance(shared_from_this(), [&](const auto &thread) {
            state.scheduler->UpdatePriority(thread);
        });
    }
}
