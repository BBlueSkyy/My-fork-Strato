// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <unistd.h>
#include <common/signal.h>
#include <common/trace.h>
#include "types/KThread.h"
#include "scheduler.h"

namespace skyline::kernel {
    namespace {
        const int ConstanceDiagnosticSignal{SIGRTMIN + 2};

        void ConstanceDiagnosticGuestSignalHandler(int signal, siginfo *info, ucontext *ctx, void **tls) {
            const auto &deviceState{*reinterpret_cast<nce::ThreadContext *>(*tls)->state};
            const auto &thread{deviceState.thread};

            LOGW("SCHEDDBG-CONSTANCE-PC: T{} signal sample in GUEST | PC=0x{:X} | SP=0x{:X} | LR=0x{:X} | coreId={} | priority={} | affinity={}",
                 thread->id,
                 ctx->uc_mcontext.pc,
                 ctx->uc_mcontext.sp,
                 ctx->uc_mcontext.regs[30],
                 thread->coreId,
                 thread->priority.load(),
                 thread->affinityMask.to_string());
        }

        void ConstanceDiagnosticHostSignalHandler(int signal, siginfo *info, ucontext *ctx) {
            LOGW("SCHEDDBG-CONSTANCE-PC: T28 diagnostic signal landed in HOST code | hostTid={} | PC=0x{:X} | SP=0x{:X} | LR=0x{:X}",
                 gettid(),
                 ctx->uc_mcontext.pc,
                 ctx->uc_mcontext.sp,
                 ctx->uc_mcontext.regs[30]);
        }
    }

    Scheduler::CoreContext::CoreContext(u8 id, i8 preemptionPriority) : id(id), preemptionPriority(preemptionPriority) {}

    Scheduler::Scheduler(const DeviceState &state) : state(state) {
        LOGW("SCHEDDBG-CONSTANCE: C0/C1/C2 queue + T28 live-PC instrumentation active");
        // Don't restart syscalls: we want futexes to fail and their predicates rechecked
        signal::SetGuestSignalHandler({Scheduler::YieldSignal, Scheduler::PreemptionSignal}, Scheduler::GuestSignalHandler, false);
        signal::SetHostSignalHandler({Scheduler::YieldSignal, Scheduler::PreemptionSignal}, Scheduler::HostSignalHandler, false);

        // Diagnostic-only signal. It samples the interrupted PC and immediately returns;
        // it does not call Rotate(), change queue order, or alter scheduler priorities.
        signal::SetGuestSignalHandler({ConstanceDiagnosticSignal}, ConstanceDiagnosticGuestSignalHandler, true);
        signal::SetHostSignalHandler({ConstanceDiagnosticSignal}, ConstanceDiagnosticHostSignalHandler, true);
    }

    void Scheduler::GuestSignalHandler(int signal, siginfo *info, ucontext *ctx, void **tls) {
        TRACE_EVENT_END("guest");
        {
            TRACE_EVENT_FMT("scheduler", "{} Signal", signal == PreemptionSignal ? "Preemption" : "Yield");
            const auto &state{*reinterpret_cast<nce::ThreadContext *>(*tls)->state};
            if (signal == PreemptionSignal)
                state.thread->isPreempted = false;
            state.scheduler->Rotate(false);
            YieldPending = false;
            state.scheduler->WaitSchedule();
        }
        TRACE_EVENT_BEGIN("guest", "Guest");
    }

    void Scheduler::HostSignalHandler(int signal, siginfo *info, ucontext *ctx) {
        YieldPending = true;
    }

    Scheduler::CoreContext &Scheduler::GetOptimalCoreForThread(const std::shared_ptr<type::KThread> &thread) {
        auto *currentCore{&cores.at(thread->coreId)};

        if (!currentCore->queue.empty() && thread->affinityMask.count() != 1) {
            // Select core where the current thread will be scheduled the earliest based off average timeslice durations for resident threads
            // There's a preference for the current core as migration isn't free
            size_t minTimeslice{};
            CoreContext *optimalCore{};
            for (auto &candidateCore : cores) {
                if (thread->affinityMask.test(candidateCore.id)) {
                    u64 timeslice{};

                    if (!candidateCore.queue.empty()) {
                        std::scoped_lock coreLock{candidateCore.mutex};

                        auto threadIterator{candidateCore.queue.cbegin()};
                        if (threadIterator != candidateCore.queue.cend()) {
                            const auto &runningThread{*threadIterator};
                            timeslice += [&]() {
                                if (runningThread->averageTimeslice)
                                    return std::min(runningThread->averageTimeslice - (util::GetTimeTicks() - runningThread->timesliceStart), 1UL);
                                else if (runningThread->timesliceStart)
                                    return util::GetTimeTicks() - runningThread->timesliceStart;
                                else
                                    return 1UL;
                            }();

                            while (++threadIterator != candidateCore.queue.cend()) {
                                const auto &residentThread{*threadIterator};
                                if (residentThread->priority <= thread->priority)
                                    timeslice += residentThread->averageTimeslice ? residentThread->averageTimeslice : 1UL;
                            }
                        }
                    }

                    if (!optimalCore || timeslice < minTimeslice || (timeslice == minTimeslice && &candidateCore == currentCore)) {
                        optimalCore = &candidateCore;
                        minTimeslice = timeslice;
                    }
                }
            }

            if (optimalCore != currentCore)
                LOGD("Load Balancing T{}: C{} -> C{}", thread->id, currentCore->id, optimalCore->id);
            else
                LOGD("Load Balancing T{}: C{} (Late)", thread->id, currentCore->id);

            return *optimalCore;
        }

        LOGD("Load Balancing T{}: C{} (Early)", thread->id, currentCore->id);

        return *currentCore;
    }

    void Scheduler::YieldThread(const std::shared_ptr<type::KThread> &thread) {
        if (state.thread != thread) {
            // If another thread is being yielded, we need to send it an OS signal to yield
            if (!thread->pendingYield) {
                // We only want to yield the thread if it hasn't already been sent a signal to yield in the past
                // Not doing this can lead to races and deadlocks but is also slower as it prevents redundant signals
                thread->SendSignal(YieldSignal);
                thread->pendingYield = true;
            }
        } else {
            // If the calling thread is being yielded, we can just set the YieldPending flag
            // This avoids an OS signal which would just flip the YieldPending flag but with significantly more overhead
            YieldPending = true;
        }
    }

    void Scheduler::InsertThread(const std::shared_ptr<type::KThread> &thread) {
        std::scoped_lock migrationLock{thread->coreMigrationMutex};
        auto &core{cores.at(thread->coreId)};
        std::unique_lock lock{core.mutex};

        if (thread->isPaused) {
            // We cannot insert a thread that is paused, so we just let the resuming thread insert it
            thread->insertThreadOnResume = true;
            return;
        }

        #ifndef NDEBUG
        // Scan the queue for the same thread to prevent double insertion
        for (auto &residentThread : core.queue) {
            if (residentThread == thread) {
                LOGE("T{} already exists in C{}", thread->id, core.id);
            }
        }
        #endif

        auto nextThread{std::upper_bound(core.queue.begin(), core.queue.end(), thread->priority.load(), type::KThread::IsHigherPriority)};
        if (nextThread == core.queue.begin()) {
            if (nextThread != core.queue.end()) {
                // If the inserted thread has a higher priority than the currently running thread (and the queue isn't empty)
                // We can yield the thread which is currently scheduled on the core by sending it a signal
                // It is optimized to avoid waiting for the thread to yield on receiving the signal which serializes the entire pipeline
                auto front{core.queue.front()};
                front->forceYield = true;
                core.queue.splice(std::upper_bound(core.queue.begin(), core.queue.end(), front->priority.load(), type::KThread::IsHigherPriority), core.queue, core.queue.begin());
                core.queue.push_front(thread);

                YieldThread(front);
            } else {
                core.queue.push_front(thread);
            }
            if (thread != state.thread)
                thread->scheduleCondition.notify(); // We only want to trigger the conditional variable if the current thread isn't inserting itself
        } else {
            core.queue.insert(nextThread, thread);
        }
    }

    void Scheduler::MigrateToCore(const std::shared_ptr<type::KThread> &thread, CoreContext *&currentCore, CoreContext *targetCore, std::unique_lock<SpinLock> &lock) {
        // We need to check if the thread was in its resident core's queue
        // If it was, we need to remove it from the queue
        auto it{std::find(currentCore->queue.begin(), currentCore->queue.end(), thread)};
        bool wasInserted{it != currentCore->queue.end()};
        if (wasInserted) {
            it = currentCore->queue.erase(it);
            if (it == currentCore->queue.begin() && it != currentCore->queue.end())
                (*it)->scheduleCondition.notify();
        }
        lock.unlock();

        thread->coreId = targetCore->id;
        if (wasInserted)
            // We need to add the thread to the ideal core queue, if it was previously its resident core's queue
            InsertThread(thread);

        currentCore = targetCore;
        lock = std::unique_lock(targetCore->mutex);
    }

    void Scheduler::WaitSchedule(bool loadBalance) {
        auto &thread{state.thread};
        CoreContext *core{&cores.at(thread->coreId)};
        std::unique_lock lock(core->mutex);

        auto wakeFunction{[&]() {
            if (!thread->affinityMask.test(thread->coreId)) [[unlikely]] {
                lock.unlock(); // If the core migration mutex is locked by a thread seeking the core mutex, it'll result in a deadlock
                std::scoped_lock migrationLock{thread->coreMigrationMutex};
                lock.lock();
                if (!thread->affinityMask.test(thread->coreId)) // We need to retest in case the thread was migrated while the core was unlocked
                    MigrateToCore(thread, core, &cores.at(thread->idealCore), lock);
            }
            return !core->queue.empty() && core->queue.front() == thread;
        }};

        TRACE_EVENT("scheduler", "WaitSchedule");
        if (loadBalance) {
            std::chrono::milliseconds loadBalanceThreshold{PreemptiveTimeslice * 2}; //!< The amount of time that needs to pass unscheduled for a thread to attempt load balancing
            while (!thread->scheduleCondition.wait_for(lock, loadBalanceThreshold, wakeFunction)) {
                std::shared_ptr<type::KThread> fairnessYieldTarget{};

                const bool constanceWaiter{thread->id == 32 || thread->id == 33};
                if (constanceWaiter) {
                    LOGW("SCHEDDBG-CONSTANCE: T{} wait timeout | resident=C{} | priority={} | preemptionPriority={} | queueSize={} | affinity={} | idealCore={} | pendingYield={} | forceYield={} | isPreempted={} | timesliceStart={} | averageTimeslice={}",
                         thread->id,
                         core->id,
                         thread->priority.load(),
                         core->preemptionPriority,
                         core->queue.size(),
                         thread->affinityMask.to_string(),
                         thread->idealCore,
                         thread->pendingYield,
                         thread->forceYield,
                         thread->isPreempted,
                         thread->timesliceStart,
                         thread->averageTimeslice);
                }

                if (!core->queue.empty()) {
                    const auto &front{core->queue.front()};
                    if (front != thread && front->priority.load() == thread->priority.load())
                        fairnessYieldTarget = front;
                }

                lock.unlock(); // We cannot call GetOptimalCoreForThread without relinquishing the core mutex

                if (constanceWaiter) {
                    std::shared_ptr<type::KThread> t28SampleTarget{};

                    // Snapshot all application cores independently while holding no other core lock.
                    // This is diagnostic-only and intentionally avoids changing queue state.
                    for (size_t coreIndex{}; coreIndex < 3; ++coreIndex) {
                        auto &diagnosticCore{cores.at(coreIndex)};
                        std::unique_lock diagnosticLock{diagnosticCore.mutex};

                        LOGW("SCHEDDBG-CONSTANCE-ALLCORES: C{} | preemptionPriority={} | queueSize={}",
                             diagnosticCore.id,
                             diagnosticCore.preemptionPriority,
                             diagnosticCore.queue.size());

                        size_t position{};
                        for (const auto &residentThread : diagnosticCore.queue) {
                            const bool isWaitingThread{residentThread == thread};
                            const bool isT28{residentThread->id == 28};

                            if (isT28)
                                t28SampleTarget = residentThread;

                            LOGW("SCHEDDBG-CONSTANCE-ALLCORES: C{} queue[{}] = T{} | priority={} | coreId={} | affinity={} | idealCore={} | pendingYield={} | forceYield={} | isPreempted={} | timesliceStart={} | averageTimeslice={}{}{}{}",
                                 diagnosticCore.id,
                                 position,
                                 residentThread->id,
                                 residentThread->priority.load(),
                                 residentThread->coreId,
                                 residentThread->affinityMask.to_string(),
                                 residentThread->idealCore,
                                 residentThread->pendingYield,
                                 residentThread->forceYield,
                                 residentThread->isPreempted,
                                 residentThread->timesliceStart,
                                 residentThread->averageTimeslice,
                                 position == 0 ? " [FRONT]" : "",
                                 isWaitingThread ? " [WAITER]" : "",
                                 isT28 ? " [T28]" : "");
                            ++position;
                        }

                        if (diagnosticCore.queue.empty())
                            LOGW("SCHEDDBG-CONSTANCE-ALLCORES: C{} queue EMPTY", diagnosticCore.id);
                    }

                    // T32 is used as the single sampler so T32/T33 do not send duplicate signals
                    // for the same scheduling state. The signal handler only records the live PC.
                    if (thread->id == 32) {
                        if (t28SampleTarget) {
                            LOGW("SCHEDDBG-CONSTANCE-PC: requesting live PC sample from T28 | coreId={} | priority={} | affinity={}",
                                 t28SampleTarget->coreId,
                                 t28SampleTarget->priority.load(),
                                 t28SampleTarget->affinityMask.to_string());
                            t28SampleTarget->SendSignal(ConstanceDiagnosticSignal);
                        } else {
                            LOGW("SCHEDDBG-CONSTANCE-PC: T28 not found in C0/C1/C2 queues");
                        }
                    }
                }

                // A runnable thread can otherwise starve indefinitely behind a same-priority thread.
                // Yielding the current front lets Rotate() preserve priority ordering while giving the
                // waiting peer a scheduling opportunity, without changing the special HOS preemption priorities.
                if (fairnessYieldTarget)
                    YieldThread(fairnessYieldTarget);

                std::scoped_lock migrationLock{thread->coreMigrationMutex};
                auto newCore{&GetOptimalCoreForThread(state.thread)};
                lock.lock();
                if (core != newCore)
                    MigrateToCore(thread, core, newCore, lock);

                loadBalanceThreshold *= 2; // We double the duration required for future load balancing for this invocation to minimize pointless load balancing
            }
        } else {
            thread->scheduleCondition.wait(lock, wakeFunction);
        }

        if (thread->priority == core->preemptionPriority)
            // If the thread needs to be preempted then arm its preemption timer
            thread->ArmPreemptionTimer(PreemptiveTimeslice);

        thread->timesliceStart = util::GetTimeTicks();
    }

    bool Scheduler::TimedWaitSchedule(std::chrono::nanoseconds timeout) {
        auto &thread{state.thread};
        auto *core{&cores.at(thread->coreId)};

        TRACE_EVENT("scheduler", "TimedWaitSchedule");
        std::unique_lock lock(core->mutex);
        if (thread->scheduleCondition.wait_for(lock, timeout, [&]() {
            if (!thread->affinityMask.test(thread->coreId)) [[unlikely]] {
                std::scoped_lock migrationLock{thread->coreMigrationMutex};
                MigrateToCore(thread, core, &cores.at(thread->idealCore), lock);
            }
            return !core->queue.empty() && core->queue.front() == thread;
        })) {
            if (thread->priority == core->preemptionPriority)
                thread->ArmPreemptionTimer(PreemptiveTimeslice);

            thread->timesliceStart = util::GetTimeTicks();

            return true;
        } else {
            return false;
        }
    }

    void Scheduler::Rotate(bool cooperative) {
        auto &thread{state.thread};
        auto &core{cores.at(thread->coreId)};

        std::unique_lock lock(core.mutex);

        if (core.queue.front() == thread) {
            // If this thread is at the front of the thread queue then we need to rotate the thread
            // In the case where this thread was forcefully yielded, we don't need to do this as it's done by the thread which yielded to this thread
            // Splice the linked element from the beginning of the queue to where its priority is present
            core.queue.splice(std::upper_bound(core.queue.begin(), core.queue.end(), thread->priority.load(), type::KThread::IsHigherPriority), core.queue, core.queue.begin());

            auto &front{core.queue.front()};
            if (front != thread)
                front->scheduleCondition.notify(); // If we aren't at the front of the queue, only then should we wake the thread at the front up
        } else if (!thread->forceYield) {
            throw exception("T{} called Rotate while not being in C{}'s queue", thread->id, thread->coreId);
        }

        thread->averageTimeslice = (thread->averageTimeslice / 4) + (3 * (util::GetTimeTicks() - thread->timesliceStart / 4));

        thread->DisarmPreemptionTimer(); // If a preemptive thread did a cooperative yield then we need to disarm the preemptive timer
        thread->pendingYield = false;
        thread->forceYield = false;
    }

    void Scheduler::RemoveThread() {
        auto &thread{state.thread};
        {
            auto &core{cores.at(thread->coreId)};
            std::unique_lock lock(core.mutex);

            if (!thread->isPaused) {
                auto it{std::find(core.queue.begin(), core.queue.end(), thread)};
                if (it != core.queue.end()) {
                    it = core.queue.erase(it);
                    if (it == core.queue.begin()) {
                        // We need to update the averageTimeslice accordingly, if we've been unscheduled by this
                        if (thread->timesliceStart)
                            thread->averageTimeslice = (thread->averageTimeslice / 4) + (3 * (util::GetTimeTicks() - thread->timesliceStart / 4));

                        if (it != core.queue.end())
                            (*it)->scheduleCondition.notify(); // We need to wake the thread at the front of the queue, if we were at the front previously
                    }
                } else {
                    LOGW("T{} was not in C{}'s queue", thread->id, thread->coreId);
                }
            } else {
                thread->insertThreadOnResume = false;
            }
        }

        thread->DisarmPreemptionTimer();
        thread->pendingYield = false;
        thread->forceYield = false;
        YieldPending = false;
    }

    void Scheduler::UpdatePriority(const std::shared_ptr<type::KThread> &thread) {
        std::scoped_lock migrationLock{thread->coreMigrationMutex};
        auto *core{&cores.at(thread->coreId)};
        std::unique_lock coreLock{core->mutex};

        auto currentIt{std::find(core->queue.begin(), core->queue.end(), thread)}, nextIt{std::next(currentIt)};
        if (currentIt == core->queue.end()) {
            return;
        } else if (currentIt == core->queue.begin()) {
            // Alternatively, if it's currently running then we'd just want to yield if there's a higher priority thread to run instead
            if (nextIt != core->queue.end() && (*nextIt)->priority < thread->priority) {
                YieldThread(thread);
            } else if (!thread->isPreempted && thread->priority == core->preemptionPriority) {
                // If the thread needs to be preempted due to its new priority then arm its preemption timer
                thread->ArmPreemptionTimer(PreemptiveTimeslice);
            } else if (thread->isPreempted && thread->priority != core->preemptionPriority) {
                // If the thread no longer needs to be preempted due to its new priority then disarm the preemptive timer
                thread->DisarmPreemptionTimer();
            }
        } else if (thread->priority < (*std::prev(currentIt))->priority || (nextIt != core->queue.end() && thread->priority > (*nextIt)->priority)) {
            // If the thread is in the queue and it's position is affected by the priority change then need to remove and re-insert the thread
            core->queue.erase(currentIt);

            auto targetIt{std::upper_bound(core->queue.begin(), core->queue.end(), thread->priority.load(), type::KThread::IsHigherPriority)};
            if (targetIt == core->queue.begin() && targetIt != core->queue.end()) {
                core->queue.insert(std::next(core->queue.begin()), thread);
                YieldThread(core->queue.front());
            } else {
                core->queue.insert(targetIt, thread);
            }
        }
    }

    void Scheduler::UpdateCore(const std::shared_ptr<type::KThread> &thread) {
        auto *core{&cores.at(thread->coreId)};
        std::scoped_lock coreLock{core->mutex};
        if (core->queue.front() == thread)
            thread->SendSignal(YieldSignal);
        else
            thread->scheduleCondition.notify();
    }

    void Scheduler::ParkThread() {
        auto &thread{state.thread};
        std::scoped_lock migrationLock{thread->coreMigrationMutex};
        RemoveThread();

        auto originalCoreId{thread->coreId};
        thread->coreId = constant::ParkedCoreId;
        for (auto &core : cores)
            if (originalCoreId != core.id && thread->affinityMask.test(core.id) && (core.queue.empty() || core.queue.front()->priority > thread->priority))
                thread->coreId = core.id;

        if (thread->coreId == constant::ParkedCoreId) {
            std::unique_lock lock(parkedMutex);
            parkedQueue.insert(std::upper_bound(parkedQueue.begin(), parkedQueue.end(), thread->priority.load(), type::KThread::IsHigherPriority), thread);
            thread->scheduleCondition.wait(lock, [&]() { return parkedQueue.front() == thread && thread->coreId != constant::ParkedCoreId; });
        }

        InsertThread(thread);
    }

    void Scheduler::WakeParkedThread() {
        std::unique_lock parkedLock(parkedMutex);
        if (!parkedQueue.empty()) {
            auto &thread{state.thread};
            auto &core{cores.at(thread->coreId)};
            std::unique_lock coreLock(core.mutex);
            auto nextThread{core.queue.size() > 1 ? *std::next(core.queue.begin()) : nullptr};
            nextThread = nextThread->priority == thread->priority ? nextThread : nullptr; // If the next thread doesn't have the same priority then it won't be scheduled next
            auto parkedThread{parkedQueue.front()};

            // We need to be conservative about waking up a parked thread, it should only be done if its priority is higher than the current thread
            // Alternatively, it should be done if its priority is equivalent to the current thread's priority but the next thread had been scheduled prior or if there is no next thread (Current thread would be rescheduled)
            if (parkedThread->priority < thread->priority || (parkedThread->priority == thread->priority && (!nextThread || parkedThread->timesliceStart < nextThread->timesliceStart))) {
                parkedThread->coreId = thread->coreId;
                parkedLock.unlock();
                parkedThread->scheduleCondition.notify();
            }
        }
    }

    void Scheduler::PauseThread(const std::shared_ptr<type::KThread> &thread) {
        CoreContext *core{&cores.at(thread->coreId)};
        std::unique_lock lock{core->mutex};

        thread->isPaused = true;

        auto it{std::find(core->queue.begin(), core->queue.end(), thread)};
        if (it != core->queue.end()) {
            thread->insertThreadOnResume = true; // If we're handling removing the thread then we need to be responsible for inserting it back inside ResumeThread

            it = core->queue.erase(it);
            if (it == core->queue.begin() && it != core->queue.end())
                (*it)->scheduleCondition.notify();

            if (it == core->queue.begin()) {
                // We need to send a yield signal to the thread if it's currently running
                YieldThread(thread);
                thread->forceYield = true;
            }
        } else {
            // If removal of the thread was performed by a lock/sleep/etc then we don't need to handle inserting it back ourselves inside ResumeThread
            // It'll be automatically re-inserted when the lock/sleep is completed and InsertThread will block till the thread is resumed
            thread->insertThreadOnResume = false;
        }
    }

    void Scheduler::ResumeThread(const std::shared_ptr<type::KThread> &thread) {
        thread->isPaused = false;
        if (thread->insertThreadOnResume)
            // If we handled removing the thread then we need to be responsible for inserting it back as well
            InsertThread(thread);
        else
            // If we're not inserting the thread back into the queue ourselves then we need to notify the thread inserting it about the updated pause state
            thread->scheduleCondition.notify();
    }
}
