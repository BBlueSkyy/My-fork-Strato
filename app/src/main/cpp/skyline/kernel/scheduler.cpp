// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <unistd.h>
#include <nce.h>
#include <common/signal.h>
#include <common/trace.h>
#include "types/KThread.h"
#include "scheduler.h"

namespace skyline::kernel {
    Scheduler::CoreContext::CoreContext(u8 id, i8 preemptionPriority) : id(id), preemptionPriority(preemptionPriority) {}

    Scheduler::Scheduler(const DeviceState &state) : state(state) {
        // Don't restart syscalls: we want futexes to fail and their predicates rechecked
        signal::SetGuestSignalHandler({Scheduler::YieldSignal, Scheduler::PreemptionSignal}, Scheduler::GuestSignalHandler, false);
        signal::SetHostSignalHandler({Scheduler::YieldSignal, Scheduler::PreemptionSignal}, Scheduler::HostSignalHandler, false);
    }

    void Scheduler::GuestSignalHandler(int signal, siginfo *info, ucontext *ctx, void **tls) {
        TRACE_EVENT_END("guest");
        try {
            TRACE_EVENT_FMT("scheduler", "{} Signal", signal == PreemptionSignal ? "Preemption" : "Yield");
            const auto &state{*reinterpret_cast<nce::ThreadContext *>(*tls)->state};
            if (signal == PreemptionSignal)
                state.thread->isPreempted = false;
            YieldPending = false;
            state.scheduler->Rotate(false);
            state.scheduler->WaitSchedule();
        } catch (const nce::NCE::ExitException &) {
            nce::NCE::SignalHandler(SIGINT, info, ctx, tls);
            return;
        }
        TRACE_EVENT_BEGIN("guest", "Guest");
    }

    void Scheduler::HostSignalHandler(int signal, siginfo *info, ucontext *ctx) {
        YieldPending = true;
    }

    Scheduler::CoreContext &Scheduler::GetOptimalCoreForThread(const std::shared_ptr<type::KThread> &thread) {
        CoreContext *optimalCore{};
        u64 minTimeslice{};
        for (auto &candidate : cores) {
            if (!thread->affinityMask.test(candidate.id))
                continue;
            std::scoped_lock lock{candidate.mutex};
            u64 cost{};
            for (const auto &resident : candidate.queue) {
                if (resident == thread || resident->priority > thread->priority)
                    continue;
                u64 duration{std::max<u64>(resident->averageTimeslice, 1)};
                if (resident == candidate.queue.front() && resident->timesliceStart) {
                    auto elapsed{util::GetTimeTicks() - resident->timesliceStart};
                    duration = elapsed < duration ? duration - elapsed : 1;
                }
                cost += std::min(duration, std::numeric_limits<u64>::max() - cost);
            }
            if (!optimalCore || cost < minTimeslice || (cost == minTimeslice && candidate.id == thread->coreId)) {
                optimalCore = &candidate;
                minTimeslice = cost;
            }
        }
        if (!optimalCore)
            throw exception("Thread {} has no permitted core", thread->id);
        return *optimalCore;
    }

    void Scheduler::YieldThread(const std::shared_ptr<type::KThread> &thread) {
        if (thread->killed)
            return;
        if (state.thread == thread) {
            YieldPending = true;
        } else if (!thread->pendingYield.exchange(true)) {
            // Publish before sending: the recipient can process the signal immediately.
            if (!thread->SendSignal(YieldSignal))
                thread->pendingYield = false;
        }
    }

    void Scheduler::InsertThread(const std::shared_ptr<type::KThread> &thread) {
        std::scoped_lock migrationLock{thread->coreMigrationMutex};
        if (thread->killed || thread->queuedCore)
            return;
        if (thread->coreId == constant::ParkedCoreId) {
            std::scoped_lock parkedLock{parkedMutex};
            parkedQueue.remove(thread);
        }
        if (thread->coreId >= constant::CoreCount || !thread->affinityMask.test(thread->coreId))
            thread->coreId = GetOptimalCoreForThread(thread).id;
        auto &core{cores.at(thread->coreId)};
        std::unique_lock lock{core.mutex};

        if (thread->isPaused) {
            // We cannot insert a thread that is paused, so we just let the resuming thread insert it
            thread->insertThreadOnResume = true;
            return;
        }

        thread->queuedCore = core.id;
        thread->insertThreadOnResume = false;

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

        thread->queuedCore.reset();
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
                core = &cores.at(thread->coreId);
                lock = std::unique_lock(core->mutex);
                if (!thread->affinityMask.test(thread->coreId)) {
                    auto target{thread->idealCore >= 0 && thread->affinityMask.test(thread->idealCore) ? thread->idealCore : std::countr_zero(thread->affinityMask.to_ullong())};
                    MigrateToCore(thread, core, &cores.at(target), lock);
                }
            }
            return thread->killed || (!thread->isPaused && !core->queue.empty() && core->queue.front() == thread);
        }};

        TRACE_EVENT("scheduler", "WaitSchedule");
        if (loadBalance) {
            std::chrono::milliseconds loadBalanceThreshold{PreemptiveTimeslice * 2}; //!< The amount of time that needs to pass unscheduled for a thread to attempt load balancing
            while (!thread->scheduleCondition.wait_for(lock, loadBalanceThreshold, wakeFunction)) {
                lock.unlock(); // We cannot call GetOptimalCoreForThread without relinquishing the core mutex
                std::scoped_lock migrationLock{thread->coreMigrationMutex};
                auto newCore{&GetOptimalCoreForThread(state.thread)};
                lock.lock();
                if (core != newCore)
                    MigrateToCore(thread, core, newCore, lock);

                if (loadBalanceThreshold.count() <= std::chrono::milliseconds::max().count() / 2)
                    loadBalanceThreshold *= 2;
            }
        } else {
            thread->scheduleCondition.wait(lock, wakeFunction);
        }

        if (thread->killed)
            throw nce::NCE::ExitException(false);
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
                lock.unlock();
                std::scoped_lock migrationLock{thread->coreMigrationMutex};
                core = &cores.at(thread->coreId);
                lock = std::unique_lock(core->mutex);
                if (!thread->affinityMask.test(thread->coreId)) {
                    auto target{thread->idealCore >= 0 && thread->affinityMask.test(thread->idealCore) ? thread->idealCore : std::countr_zero(thread->affinityMask.to_ullong())};
                    MigrateToCore(thread, core, &cores.at(target), lock);
                }
            }
            return thread->killed || (!thread->isPaused && !core->queue.empty() && core->queue.front() == thread);
        })) {
            if (thread->killed)
                throw nce::NCE::ExitException(false);
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

        if (!core.queue.empty() && core.queue.front() == thread) {
            // If this thread is at the front of the thread queue then we need to rotate the thread
            // In the case where this thread was forcefully yielded, we don't need to do this as it's done by the thread which yielded to this thread
            // Splice the linked element from the beginning of the queue to where its priority is present
            core.queue.splice(std::upper_bound(core.queue.begin(), core.queue.end(), thread->priority.load(), type::KThread::IsHigherPriority), core.queue, core.queue.begin());

            auto &front{core.queue.front()};
            if (front != thread)
                front->scheduleCondition.notify(); // If we aren't at the front of the queue, only then should we wake the thread at the front up
        } // A late yield may arrive after the thread was removed or paused.

        if (thread->timesliceStart)
            thread->averageTimeslice = thread->averageTimeslice / 4 + 3 * ((util::GetTimeTicks() - thread->timesliceStart) / 4);

        thread->DisarmPreemptionTimer(); // If a preemptive thread did a cooperative yield then we need to disarm the preemptive timer
        thread->pendingYield = false;
        thread->forceYield = false;
        lock.unlock();
        WakeParkedThread();
    }

    void Scheduler::RemoveThread() {
        auto &thread{state.thread};
        {
            std::scoped_lock migrationLock{thread->coreMigrationMutex};
            if (thread->queuedCore) {
                auto &core{cores.at(*thread->queuedCore)};
                std::scoped_lock lock{core.mutex};
                bool wasFront{!core.queue.empty() && core.queue.front() == thread};
                core.queue.remove(thread);
                thread->queuedCore.reset();
                if (wasFront && !core.queue.empty())
                    core.queue.front()->scheduleCondition.notify();
                if (thread->timesliceStart)
                    thread->averageTimeslice = thread->averageTimeslice / 4 + 3 * ((util::GetTimeTicks() - thread->timesliceStart) / 4);
                thread->timesliceStart = 0;
            }
            if (thread->coreId == constant::ParkedCoreId) {
                std::scoped_lock parkedLock{parkedMutex};
                parkedQueue.remove(thread);
            }
            thread->insertThreadOnResume = false;
            thread->DisarmPreemptionTimer();
            thread->pendingYield = false;
            thread->forceYield = false;
            YieldPending = false;
        }
        if (thread->coreId < constant::CoreCount)
            WakeParkedThread();
    }

    void Scheduler::UpdatePriority(const std::shared_ptr<type::KThread> &thread) {
        std::scoped_lock migrationLock{thread->coreMigrationMutex};
        if (!thread->queuedCore)
            return;
        auto &core{cores.at(*thread->queuedCore)};
        std::scoped_lock lock{core.mutex};
        if (core.queue.empty())
            return;
        auto previous{core.queue.front()};
        core.queue.sort([](const auto &a, const auto &b) { return a->priority < b->priority; });
        if (core.queue.front() != previous) {
            previous->forceYield = true;
            YieldThread(previous);
            core.queue.front()->scheduleCondition.notify();
        } else if (core.queue.front() == thread) {
            if (!thread->isPreempted && thread->priority == core.preemptionPriority)
                thread->ArmPreemptionTimer(PreemptiveTimeslice);
            else if (thread->isPreempted && thread->priority != core.preemptionPriority)
                thread->DisarmPreemptionTimer();
        }
    }

    void Scheduler::UpdateCore(const std::shared_ptr<type::KThread> &thread) {
        if (thread->coreId == constant::ParkedCoreId) {
            thread->scheduleCondition.notify();
            return;
        }
        auto &core{cores.at(thread->coreId)};
        std::scoped_lock lock{core.mutex};
        if (!core.queue.empty() && core.queue.front() == thread)
            YieldThread(thread);
        else
            thread->scheduleCondition.notify();
    }

    void Scheduler::SetCoreMask(const std::shared_ptr<type::KThread> &thread, i32 idealCore, CoreMask affinityMask) {
        std::scoped_lock migrationLock{thread->coreMigrationMutex};
        if (thread->coreId < constant::CoreCount) {
            auto &core{cores.at(thread->coreId)};
            std::scoped_lock lock{core.mutex};
            thread->idealCore = idealCore;
            thread->affinityMask = affinityMask;
            if (!affinityMask.test(thread->coreId)) {
                if (!core.queue.empty() && core.queue.front() == thread)
                    YieldThread(thread);
                thread->scheduleCondition.notify();
            }
        } else {
            std::scoped_lock lock{parkedMutex};
            thread->idealCore = idealCore;
            thread->affinityMask = affinityMask;
            thread->coreId = GetOptimalCoreForThread(thread).id;
            parkedQueue.remove(thread);
            thread->scheduleCondition.notify();
        }
    }

    void Scheduler::ParkThread() {
        auto &thread{state.thread};
        RemoveThread();
        std::unique_lock migrationLock{thread->coreMigrationMutex};
        auto originalCoreId{thread->coreId.load()};
        std::optional<u8> target;
        for (auto &core : cores) {
            if (!thread->affinityMask.test(core.id))
                continue;
            std::scoped_lock lock{core.mutex};
            if (core.queue.empty() || (originalCoreId != core.id && core.queue.front()->priority > thread->priority)) {
                target = core.id;
                break;
            }
        }
        if (target) {
            thread->coreId = *target;
            migrationLock.unlock();
            InsertThread(thread);
            return;
        }
        thread->coreId = constant::ParkedCoreId;
        std::unique_lock lock{parkedMutex};
        parkedQueue.push_back(thread);
        migrationLock.unlock();
        thread->scheduleCondition.wait(lock, [&] { return thread->killed || thread->coreId != constant::ParkedCoreId; });
        lock.unlock();
        if (thread->killed)
            throw nce::NCE::ExitException(false);
        InsertThread(thread);
    }

    void Scheduler::WakeParkedThread() {
        if (!state.thread || state.thread->coreId >= constant::CoreCount)
            return;
        std::vector<std::shared_ptr<type::KThread>> candidates;
        {
            std::scoped_lock lock{parkedMutex};
            candidates.assign(parkedQueue.begin(), parkedQueue.end());
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a->priority < b->priority; });
        for (const auto &candidate : candidates) {
            std::scoped_lock migrationLock{candidate->coreMigrationMutex};
            std::unique_lock parkedLock{parkedMutex};
            auto it{std::find(parkedQueue.begin(), parkedQueue.end(), candidate)};
            if (it == parkedQueue.end() || candidate->isPaused || candidate->killed || !candidate->affinityMask.test(state.thread->coreId))
                continue;
            auto &core{cores.at(state.thread->coreId)};
            std::unique_lock coreLock{core.mutex};
            if (!core.queue.empty() && core.queue.front()->priority < candidate->priority)
                continue;
            candidate->coreId = core.id;
            parkedQueue.erase(it);
            coreLock.unlock();
            parkedLock.unlock();
            candidate->scheduleCondition.notify();
            return;
        }
    }

    void Scheduler::PauseThread(const std::shared_ptr<type::KThread> &thread) {
        if (thread->coreId == constant::ParkedCoreId) {
            std::scoped_lock lock{parkedMutex};
            thread->isPaused = true;
            thread->insertThreadOnResume = true;
            parkedQueue.remove(thread);
            return;
        }
        CoreContext *core{&cores.at(thread->coreId)};
        std::unique_lock lock{core->mutex};

        thread->isPaused = true;

        auto it{std::find(core->queue.begin(), core->queue.end(), thread)};
        if (it != core->queue.end()) {
            thread->insertThreadOnResume = true; // If we're handling removing the thread then we need to be responsible for inserting it back inside ResumeThread

            bool wasFront{it == core->queue.begin()};
            thread->queuedCore.reset();
            it = core->queue.erase(it);
            if (it == core->queue.begin() && it != core->queue.end())
                (*it)->scheduleCondition.notify();

            if (wasFront) {
                thread->forceYield = true;
                YieldThread(thread);
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
