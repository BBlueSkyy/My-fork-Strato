// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cxxabi.h>
#include <algorithm>
#include <unistd.h>
#include <common/signal.h>
#include <common/trace.h>
#include <nce.h>
#include "jit/jit32.h"
#include <os.h>
#include "KProcess.h"
#include "KThread.h"

namespace skyline::kernel::type {
    namespace {
        const char *DiagnosticActivityTypeName(DiagnosticActivityType type) {
            switch (type) {
                case DiagnosticActivityType::SvcExit: return "svc-exit";
                case DiagnosticActivityType::IpcExit: return "ipc-exit";
                case DiagnosticActivityType::WaitSyncBegin: return "wait-sync-begin";
                case DiagnosticActivityType::WaitSyncEnd: return "wait-sync-end";
                case DiagnosticActivityType::CondvarWaitBegin: return "condvar-wait-begin";
                case DiagnosticActivityType::CondvarWaitEnd: return "condvar-wait-end";
                case DiagnosticActivityType::CondvarSignal: return "condvar-signal";
                case DiagnosticActivityType::SyncSignal: return "sync-signal";
                case DiagnosticActivityType::SyncWake: return "sync-wake";
                default: return "unknown";
            }
        }
    }

    void KThread::RecordDiagnosticActivity(DiagnosticActivityType activityType, u32 activityId, u32 activityValue,
                                           u64 activityArg0, u64 activityArg1, const char *activityName) {
        if (priority.load(std::memory_order_relaxed) != 44)
            return;

        const u64 sequence{diagnosticActivitySequence.fetch_add(1, std::memory_order_relaxed) + 1};
        auto &slot{diagnosticActivities[(sequence - 1) % DiagnosticActivityCount]};

        slot.sequence.store((sequence << 1) | 1, std::memory_order_release);
        slot.tick.store(util::GetTimeTicks(), std::memory_order_relaxed);
        slot.type.store(static_cast<u32>(activityType), std::memory_order_relaxed);
        slot.id.store(activityId, std::memory_order_relaxed);
        slot.value.store(activityValue, std::memory_order_relaxed);
        slot.arg0.store(activityArg0, std::memory_order_relaxed);
        slot.arg1.store(activityArg1, std::memory_order_relaxed);
        slot.name.store(activityName, std::memory_order_relaxed);
        slot.sequence.store(sequence << 1, std::memory_order_release);
    }

    void KThread::LogDiagnosticActivities(const char *reason) const {
        struct Snapshot {
            u64 sequence;
            u64 tick;
            DiagnosticActivityType type;
            u32 id;
            u32 value;
            u64 arg0;
            u64 arg1;
            const char *name;
        };

        std::array<Snapshot, DiagnosticActivityCount> snapshots{};
        size_t count{};

        for (const auto &slot : diagnosticActivities) {
            const u64 before{slot.sequence.load(std::memory_order_acquire)};
            if (!before || (before & 1))
                continue;

            Snapshot snapshot{
                .sequence = before >> 1,
                .tick = slot.tick.load(std::memory_order_relaxed),
                .type = static_cast<DiagnosticActivityType>(slot.type.load(std::memory_order_relaxed)),
                .id = slot.id.load(std::memory_order_relaxed),
                .value = slot.value.load(std::memory_order_relaxed),
                .arg0 = slot.arg0.load(std::memory_order_relaxed),
                .arg1 = slot.arg1.load(std::memory_order_relaxed),
                .name = slot.name.load(std::memory_order_relaxed),
            };

            const u64 after{slot.sequence.load(std::memory_order_acquire)};
            if (before != after || (after & 1))
                continue;

            snapshots[count++] = snapshot;
        }

        std::sort(snapshots.begin(), snapshots.begin() + count, [](const auto &lhs, const auto &rhs) {
            return lhs.sequence < rhs.sequence;
        });

        LOGI("[THREAD-ACT] T{} reason={} entries={}", id, reason ? reason : "<none>", count);
        for (size_t i{}; i < count; i++) {
            const auto &entry{snapshots[i]};
            LOGI("[THREAD-ACT] T{} seq={} tick={} type={} id=0x{:X} value=0x{:X} arg0=0x{:X} arg1=0x{:X} name={}",
                 id, entry.sequence, entry.tick, DiagnosticActivityTypeName(entry.type), entry.id, entry.value,
                 entry.arg0, entry.arg1, entry.name ? entry.name : "<none>");
        }
    }

    KThread::KThread(const DeviceState &state, KHandle handle, KProcess &process, size_t id, void *entry, u64 argument, void *stackTop, i8 priority, u8 idealCore)
        : handle(handle),
          process(process),
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
        if (thread.joinable())
            thread.join();
        if (preemptionTimer)
            timer_delete(preemptionTimer);
    }

    void KThread::ThreadEntrypoint() {
        this_thread = this;
        pthread = pthread_self();
        std::array<char, 16> threadName{};
        if (int result{pthread_getname_np(pthread, threadName.data(), threadName.size())})
            LOGW("Failed to get the thread name: {}", strerror(result));

        if (int result{pthread_setname_np(pthread, fmt::format("HOS-{}", id).c_str())})
            LOGW("Failed to set the thread name: {}", strerror(result));
        AsyncLogger::UpdateTag();

        if (!tlsRegion)
            tlsRegion = process.AllocateTlsSlot();

        state.thread = shared_from_this();

        LOGI("[THREAD-DIAG] T{} host entrypoint started core={} ideal={} entry={} stack={} tls={}",
             id, coreId, idealCore, entry, fmt::ptr(stackTop), fmt::ptr(tlsRegion));

        if (setjmp(originalCtx)) { // Returns 1 if it's returning from guest, 0 otherwise
            state.scheduler->RemoveThread();

            {
                std::scoped_lock lock{statusMutex};
                running = false;
                ready = false;
                statusCondition.notify_all();
            }

            Signal();
            this_thread = nullptr;

            // Restore the previous thread name if any
            if (threadName[0] != 'H' || threadName[1] != 'O' || threadName[2] != 'S' || threadName[3] != '-') {
                if (int result{pthread_setname_np(pthread, threadName.data())})
                    LOGW("Failed to set the thread name: {}", strerror(result));
                AsyncLogger::UpdateTag();
            }

            return;
        }

        struct sigevent event{
            .sigev_signo = Scheduler::PreemptionSignal,
            .sigev_notify = SIGEV_THREAD_ID,
            .sigev_notify_thread_id = gettid(),
        };
        if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &preemptionTimer))
            throw exception("timer_create has failed with '{}'", strerror(errno));

        // Initialize execution-mode-specific stuff
        Init();
        LOGI("[THREAD-DIAG] T{} Init complete core={} YieldPending={}", id, coreId, Scheduler::YieldPending);

        {
            std::scoped_lock lock{statusMutex};
            ready = true;
            statusCondition.notify_all();
        }

        try {
            if (!Scheduler::YieldPending) {
                LOGI("[THREAD-DIAG] T{} initial WaitSchedule begin core={}", id, coreId);
                state.scheduler->WaitSchedule();
                LOGI("[THREAD-DIAG] T{} initial WaitSchedule returned core={}", id, coreId);
            } else {
                LOGI("[THREAD-DIAG] T{} initial WaitSchedule skipped: YieldPending=true", id);
            }

            bool firstGuestRun{true};
            while (!killed) {
                while (Scheduler::YieldPending && !killed) [[unlikely]] {
                    // If there is a yield pending on us after thread creation
                    state.scheduler->Rotate();
                    Scheduler::YieldPending = false;
                    state.scheduler->WaitSchedule();
                }

                TRACE_EVENT("guest", "Guest");
                if (firstGuestRun) {
                    LOGI("[THREAD-DIAG] T{} first guest Run dispatch core={} entry={}", id, coreId, entry);
                    firstGuestRun = false;
                }
                // Run the guest code
                Run();
            }
            // A JIT thread stopped by SIGINT returns from Dynarmic instead of jumping
            // past its execution guard; finish through the normal thread-exit path.
            throw nce::NCE::ExitException(false);
        } catch (const nce::NCE::ExitException &e) {
            // NCE handles guest exits in its SVC handler; JIT exits reach this thread boundary.
            jit = nullptr;
            if (e.killAllThreads && id) {
                signal::BlockSignal({SIGINT});
                state.process->Kill(false);
            }
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

    void KThread::Start(bool self) {
        std::unique_lock lock(statusMutex);
        if (!running) {
            LOGI("[THREAD-DIAG] T{} Start begin handle=0x{:X} entry={} ideal={} core={} priority={} self={}",
                 id, handle, entry, idealCore, coreId, priority.load(), self);
            {
                std::scoped_lock migrationLock{coreMigrationMutex};
                auto thisShared{shared_from_this()};
                coreId = state.scheduler->GetOptimalCoreForThread(thisShared).id;
                state.scheduler->InsertThread(thisShared);
                LOGI("[THREAD-DIAG] T{} Start inserted core={}", id, coreId);
            }

            running = true;
            killed = false;
            statusCondition.notify_all();
            if (self) {
                lock.unlock();
                ThreadEntrypoint();
            } else {
                thread = std::thread(&KThread::ThreadEntrypoint, this);
                LOGI("[THREAD-DIAG] T{} host std::thread created", id);
            }
        }
    }

    void KThread::Kill(bool join) {
        std::unique_lock lock(statusMutex);
        if (!killed && running) {
            statusCondition.wait(lock, [this]() { return ready || killed; });
            if (!killed) {
                killed = true;
                pthread_kill(pthread, SIGINT);
                statusCondition.notify_all();
            }
        }
        if (join)
            statusCondition.wait(lock, [this]() { return !running; });
    }

    void KThread::SendSignal(int signal) {
        std::unique_lock lock(statusMutex);
        statusCondition.wait(lock, [this]() { return ready || killed; });
        if (!killed && running) {
            LOGI("[THREAD-DIAG] T{} SendSignal signal={} core={} priority={} pendingYield={} forceYield={}",
                 id, signal, coreId, priority.load(), pendingYield, forceYield);
            pthread_kill(pthread, signal);
        }
    }

    void KThread::ArmPreemptionTimer(std::chrono::nanoseconds timeToFire) {
        std::unique_lock lock(statusMutex);
        statusCondition.wait(lock, [this]() { return ready || killed; });
        if (!killed && running) {
            LOGI("[THREAD-DIAG] T{} ArmPreemptionTimer core={} priority={} duration_ns={}",
                 id, coreId, priority.load(), timeToFire.count());
            struct itimerspec spec{.it_value = {
                .tv_nsec = std::min(static_cast<i64>(timeToFire.count()), constant::NsInSecond),
                .tv_sec = std::max(std::chrono::duration_cast<std::chrono::seconds>(timeToFire).count() - 1, 0LL),
            }};
            timer_settime(preemptionTimer, 0, &spec, nullptr);
            isPreempted = true;
        }
    }

    void KThread::DisarmPreemptionTimer() {
        if (!isPreempted) [[unlikely]]
            return;

        std::unique_lock lock(statusMutex);
        statusCondition.wait(lock, [this]() { return ready || killed; });
        if (!killed && running) {
            LOGI("[THREAD-DIAG] T{} DisarmPreemptionTimer core={} priority={}", id, coreId, priority.load());
            struct itimerspec spec{};
            timer_settime(preemptionTimer, 0, &spec, nullptr);
            isPreempted = false;
        }
    }

    void KThread::UpdatePriorityInheritance() {
        std::scoped_lock priorityLock{process.GetPriorityInheritanceMutex()};
        auto thread{shared_from_this()};

        while (thread) {
            std::unique_lock lock{thread->waiterMutex};

            // Horizon separates the requested/base priority from the effective
            // scheduler priority. The latter is the highest priority (lowest
            // numeric value) among the base priority and all mutex waiters.
            i8 newPriority{thread->basePriority.load()};
            if (!thread->waiters.empty())
                newPriority = std::min(newPriority, thread->waiters.front()->priority.load());

            const i8 oldPriority{thread->priority.load()};
            if (newPriority == oldPriority)
                return;

            auto waitingOn{thread->waitThread};
            std::unique_lock<RecursiveSpinLock> ownerLock;
            if (waitingOn) {
                // A priority change also changes this thread's ordering in its
                // owner's waiter list. Avoid lock-order deadlocks by using the
                // same retry pattern as the previous PI propagation code.
                ownerLock = std::unique_lock<RecursiveSpinLock>{waitingOn->waiterMutex, std::try_to_lock};
                if (!ownerLock) {
                    lock.unlock();

                    ownerLock.lock();
                    ownerLock.unlock();
                    continue;
                }

                auto &ownerWaiters{waitingOn->waiters};
                auto waiter{std::find(ownerWaiters.begin(), ownerWaiters.end(), thread)};
                if (waiter == ownerWaiters.end())
                    throw exception("Priority inheritance waiter missing from owner queue");

                ownerWaiters.erase(waiter);
                thread->priority = newPriority;
                ownerWaiters.insert(std::upper_bound(ownerWaiters.begin(), ownerWaiters.end(), newPriority, KThread::IsHigherPriority), thread);
            } else {
                thread->priority = newPriority;
            }

            state.scheduler->UpdatePriority(thread);

            // If this thread is itself waiting on a mutex, its changed effective
            // priority may change the priority inherited by that mutex's owner.
            thread = waitingOn;
        }
    }

    void KNceThread::Init() {
        ctx.tpidrroEl0 = tlsRegion;
        ctx.state = &state;
    }

    void KNceThread::Run() {
        LOGI("[THREAD-DIAG] T{} entering KNceThread::Run core={} entry={} argument=0x{:X} handle=0x{:X}",
             id, coreId, entry, entryArgument, handle);
        asm volatile(
            "MRS X0, TPIDR_EL0\n\t" // Retrieve current (host) TLS
            "MSR TPIDR_EL0, %x0\n\t" // Set TLS to ThreadContext
            "STR X0, [%x0, #0x2A0]\n\t" // Write host TLS to ThreadContext::hostTpidrEl0
            "MOV X0, SP\n\t" // Load the current (host) stack pointer
            "STR X0, [%x0, #0x2A8]\n\t" // Write host SP to ThreadContext::hostSp
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
    }

    void KJit32Thread::Init() {
        ctx.gpr[0] = static_cast<u32>(entryArgument);
        ctx.gpr[1] = handle;

        ctx.sp = static_cast<u32>(reinterpret_cast<uintptr_t>(stackTop));
        ctx.pc = static_cast<u32>(reinterpret_cast<uintptr_t>(entry));
    }

    void KJit32Thread::Run() {
        auto *core{&state.jit32->GetCore(coreId)};
        core->Run(ctx, static_cast<u32>(process.memory.TranslateHostAddress(tlsRegion)));
    }
}
