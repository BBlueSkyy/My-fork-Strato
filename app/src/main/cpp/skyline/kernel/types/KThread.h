// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <csetjmp>
#include <nce/guest.h>
#include <kernel/scheduler.h>
#include <common/signal.h>
#include <common/spin_lock.h>
#include "KSyncObject.h"
#include "KSharedMemory.h"

namespace skyline {
    namespace kernel::type {
        /**
         * @brief KThread manages a single thread of execution which is responsible for running guest code and kernel code which is invoked by the guest
         */
        class KThread : public KSyncObject, public std::enable_shared_from_this<KThread> {
          private:
            KProcess *parent;
            std::thread thread; //!< If this KThread is backed by a host thread then this'll hold it
            pthread_t pthread{}; //!< The pthread_t for the host thread running this guest thread
            timer_t preemptionTimer{}; //!< A kernel timer used for preemption interrupts

            /**
             * @brief Entry function any guest threads, sets up necessary context and jumps into guest code from the calling thread
             * @note This function also serves as the entry point for host threads created in StartThread
             */
            void StartThread();

          public:
            enum class GuestExecutionState : u32 {
                Kernel,
                Guest,
                PauseRequested,
                PauseAcknowledging,
                Paused,
                Terminated,
            };

            enum class GuestKernelEntry {
                Entered,
                PauseRequested,
                AlreadyEntered,
                Terminated,
            };

            enum class ContextPauseRequest {
                Stable,
                Signal,
                Terminated,
            };

            std::mutex statusMutex; //!< Synchronizes all thread state changes (running/ready/killed)
            std::condition_variable statusCondition; //!< Signalled on the status of the thread changing
            bool running{false}; //!< If the host thread that corresponds to this thread is running, this doesn't reflect guest scheduling changes
            bool ready{false}; //!< If this thread is ready to recieve signals or not
            bool killed{false}; //!< If this thread was previously running and has been killed

            KHandle handle;
            size_t id; //!< Index of thread in parent process's KThread vector

            std::array<nce::GuestCpuContext, 2> guestCpuContexts{}; //!< Double-buffered guest contexts; only a complete capture is published
            nce::ThreadContext ctx{}; //!< The context used by NCE while entering host code; its full-context pointer always targets the unpublished buffer
            jmp_buf originalCtx; //!< The context of the host thread prior to jumping into guest code

            std::atomic<nce::GuestCpuContext *> publishedGuestCpuContext{}; //!< Immutable full context exposed to GetThreadContext3
            alignas(sizeof(u32)) std::atomic<GuestExecutionState> guestExecutionState{GuestExecutionState::Kernel}; //!< Synchronizes full-context capture with pause/resume

            void *entry; //!< A function pointer to the thread's entry
            u64 entryArgument; //!< An argument to provide with to the thread entry function
            void *stackTop; //!< The top of the guest's stack, this is set to the initial guest stack pointer

            AdaptiveSingleWaiterConditionVariable scheduleCondition; //!< Signalled to wake the thread when it's scheduled or its resident core changes
            std::atomic<i8> basePriority; //!< The priority of the thread for the scheduler without any priority-inheritance
            std::atomic<i8> priority; //!< The priority of the thread for the scheduler including priority-inheritance

            std::recursive_mutex coreMigrationMutex; //!< Synchronizes operations which depend on which core the thread is running on
            u8 idealCore; //!< The ideal CPU core for this thread to run on
            u8 coreId; //!< The CPU core on which this thread is running
            CoreMask affinityMask{}; //!< A mask of CPU cores this thread is allowed to run on

            u64 timesliceStart{}; //!< A timestamp in host CNTVCT ticks of when the thread's current timeslice started
            u64 averageTimeslice{}; //!< A weighted average of the timeslice duration for this thread

            bool isPreempted{}; //!< If the preemption timer has been armed and will fire
            bool pendingYield{}; //!< If the thread has been yielded and hasn't been acted upon it yet
            bool forceYield{}; //!< If the thread has been forcefully yielded by another thread

            RecursiveSpinLock waiterMutex; //!< Synchronizes operations on mutation of the waiter members
            u32 *waitMutex; //!< The key of the mutex which this thread is waiting on
            KHandle waitTag; //!< The handle of the thread which requested the mutex lock
            std::shared_ptr<KThread> waitThread; //!< The thread which this thread is waiting on
            std::list<std::shared_ptr<type::KThread>> waiters; //!< A queue of threads waiting on this thread sorted by priority
            void *waitConditionVariable; //!< The condition variable which this thread is waiting on
            bool waitSignalled{}; //!< If the conditional variable has been signalled already
            Result waitResult; //!< The result of the wait operation

            bool isCancellable{false}; //!< If the thread is currently in a position where it's cancellable
            bool cancelSync{false}; //!< Whether to cancel the SvcWaitSynchronization call this thread currently is in/the next one it joins
            type::KSyncObject *wakeObject{}; //!< A pointer to the synchronization object responsible for waking this thread up

            std::atomic<bool> isPaused{false}; //!< If the thread is currently paused and not runnable
            bool insertThreadOnResume{false}; //!< If the thread should be inserted into the scheduler when it resumes (used for pausing threads during sleep/sync)

            KThread(const DeviceState &state, KHandle handle, KProcess *parent, size_t id, void *entry, u64 argument, void *stackTop, i8 priority, u8 idealCore);

            ~KThread();

            /**
             * @param self If the calling thread should jump directly into guest code or if a new thread should be created for it
             * @note If the thread is already running then this does nothing
             * @note 'stack' will be created if it wasn't set prior to calling this
             */
            void Start(bool self = false);

            /**
             * @param join Return after the thread has joined rather than instantly
             */
            void Kill(bool join);

            /**
             * @brief Sends a host OS signal to the thread which is running this KThread
             */
            void SendSignal(int signal);

            /**
             * @brief Arms the preemption kernel timer to fire in the specified amount of time
             */
            void ArmPreemptionTimer(std::chrono::nanoseconds timeToFire);

            /**
             * @brief Disarms the preemption kernel timer, any scheduled firings will be cancelled
             */
            void DisarmPreemptionTimer();

            /**
             * @brief Marks entry from guest execution into a stable host/kernel boundary
             * @return The result of atomically claiming this guest-to-kernel transition
             */
            GuestKernelEntry BeginGuestKernelExecution();

            /**
             * @brief Atomically claims an outstanding guest pause from a host-side signal handler
             * @return True only for the handler responsible for acknowledging and completing the pause
             */
            bool TryClaimContextPause();

            /**
             * @brief Atomically publishes the completed full-context capture and selects the old buffer for the next capture
             */
            void PublishGuestContext();

            const nce::GuestCpuContext &GetPublishedGuestContext() const;

            /**
             * @brief Returns from a host/kernel boundary to guest execution, blocking without polling while paused
             */
            void EndGuestKernelExecution();

            /**
             * @brief Requests a stable full-context pause
             */
            ContextPauseRequest RequestContextPause();

            /**
             * @brief Publishes completion of a live guest-context capture to SetThreadActivity
             * @return True if this caller completed the transition to the paused state
             */
            bool AcknowledgeContextPause();

            /**
             * @brief Blocks until a requested live guest capture is stable or the thread terminates
             */
            void WaitForContextPause();

            /**
             * @brief Releases a paused guest/kernel boundary
             */
            void ResumeContext();

            /**
             * @brief Wakes all context-state waiters when the thread exits
             */
            void MarkContextTerminated();

            bool IsContextPauseRequested() const;

            bool HasPausedContext() const;

            bool IsContextTerminated() const;

            /**
             * @brief Recursively updates the priority for any threads this thread might be waiting on
             * @note PI is performed by temporarily upgrading a thread's priority if a thread waiting on it has a higher priority to prevent priority inversion
             * @note This will lock `waiterMutex` internally and it must **not** be held when calling this function
             */
            void UpdatePriorityInheritance();

            /**
             * @return If the supplied priority value is higher than the supplied thread's priority value
             */
            static constexpr bool IsHigherPriority(const i8 priority, const std::shared_ptr<type::KThread> &it) {
                return priority < it->priority;
            }
        };
    }
}
