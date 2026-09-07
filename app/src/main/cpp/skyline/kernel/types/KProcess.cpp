// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <nce.h>
#include <os.h>
#include <jvm.h>
#include <common/trace.h>
#include <kernel/results.h>
#include <kernel/address_arbiter.h>
#include "KProcess.h"

namespace skyline::kernel::type {
    KProcess::TlsPage::TlsPage(u8 *memory) : memory(memory) {}

    u8 *KProcess::TlsPage::ReserveSlot() {
        if (index == constant::TlsSlots)
            return nullptr;
        return memory + (constant::TlsSlotSize * index++);
    }

    KProcess::KProcess(const DeviceState &state) : memory(state), KSyncObject(state, KType::KProcess) {
        trap.InstallStaticInstance();
    }

    KProcess::~KProcess() {
        std::scoped_lock guard{threadMutex};
        disableThreadCreation = true;
        for (const auto &thread : threads)
            thread->Kill(true);

        // Must happen after all threads have been killed/joined so no host thread can fault into
        // this process' trap map while (or after) it's being torn down
        trap.UninstallStaticInstance();
    }

    void KProcess::Kill(bool join, bool all, bool disableCreation) {
        // disableCreation is set only when gracefully exiting, it being false means an exception/crash occurred
        if (!disableCreation)
            state.jvm->reportCrash();

        bool expected{false};
        if (!join && !alreadyKilled.compare_exchange_strong(expected, true))
            // If the process has already been killed and we don't want to wait for it to join then just instantly return rather than waiting on the mutex
            return;
        else
            alreadyKilled.store(true);

        std::scoped_lock guard{threadMutex};
        if (disableCreation)
            disableThreadCreation = true;
        if (all) {
            for (const auto &thread : threads)
                thread->Kill(join);
        } else if (!threads.empty()) {
            threads[0]->Kill(join);
        }
    }

    void KProcess::InitializeHeapTls() {
        constexpr size_t DefaultHeapSize{0x200000};
        memory.MapHeapMemory(span<u8>{state.process->memory.heap.guest.data(), DefaultHeapSize});
        memory.processHeapSize = DefaultHeapSize;
        tlsExceptionContext = AllocateTlsSlot();
    }

    u8 *KProcess::AllocateTlsSlot() {
        std::scoped_lock lock{tlsMutex};
        u8 *slot;
        for (auto &tlsPage : tlsPages)
            if ((slot = tlsPage->ReserveSlot()))
                return slot;

        bool isAllocated{};

        u8 *pageCandidate{state.process->memory.tlsIo.guest.data()};
        while (state.process->memory.tlsIo.guest.contains(span<u8>(pageCandidate, constant::PageSize))) {
            auto chunk = memory.GetChunk(pageCandidate);
            if (!chunk)
                break;

            if (chunk->second.state == memory::states::Unmapped) {
                memory.MapThreadLocalMemory(span<u8>{pageCandidate, constant::PageSize});
                isAllocated = true;
                break;
            } else {
                pageCandidate = chunk->first + chunk->second.size;
            }
        }

        if (!isAllocated)
            throw exception("Failed to find free memory for a tls slot!");

        auto tlsPage{std::make_shared<TlsPage>(pageCandidate)};
        tlsPages.push_back(tlsPage);
        return tlsPage->ReserveSlot();
    }

    std::shared_ptr<KThread> KProcess::CreateThread(void *entry, u64 argument, void *stackTop, std::optional<i8> priority, std::optional<u8> idealCore) {
        std::scoped_lock guard{threadMutex};
        if (disableThreadCreation)
            return nullptr;
        if (!stackTop && threads.empty()) { //!< Main thread stack is created by the kernel and owned by the process
            bool isAllocated{};

            u8 *pageCandidate{memory.stack.guest.data()};
            while (state.process->memory.stack.guest.contains(span<u8>(pageCandidate, state.process->npdm.meta.mainThreadStackSize))) {
                auto chunk{memory.GetChunk(pageCandidate)};
                if (!chunk)
                    break;

                if (chunk->second.state == memory::states::Unmapped && chunk->second.size >= state.process->npdm.meta.mainThreadStackSize) {
                    memory.MapStackMemory(span<u8>{pageCandidate, state.process->npdm.meta.mainThreadStackSize});
                    isAllocated = true;
                    break;
                } else {
                    pageCandidate = chunk->first + chunk->second.size;
                }
            }

            if (!isAllocated)
                throw exception("Failed to map main thread stack!");

            stackTop = pageCandidate + state.process->npdm.meta.mainThreadStackSize;
            mainThreadStack = span<u8>(pageCandidate, state.process->npdm.meta.mainThreadStackSize);
        }
        size_t tid{threads.size() + 1}; //!< The first thread is HOS-1 rather than HOS-0, this is to match the HOS kernel's behaviour
        auto thread{NewHandle<KThread>(this, tid, entry, argument, stackTop, priority ? *priority : state.process->npdm.meta.mainThreadPriority, idealCore ? *idealCore : state.process->npdm.meta.idealCore).item};
        threads.push_back(thread);
        return thread;
    }

    void KProcess::ClearHandleTable() {
        std::shared_lock lock(handleMutex);
        handles.clear();
    }

    constexpr u32 HandleWaitersBit{1UL << 30}; //!< A bit which denotes if a mutex psuedo-handle has waiters or not

    Result KProcess::MutexLock(const std::shared_ptr<KThread> &thread, u32 *mutex, KHandle ownerHandle, KHandle tag, bool failOnOutdated) {
        TRACE_EVENT_FMT("kernel", "MutexLock {} @ 0x{:X}", fmt::ptr(mutex), thread->id);
        {
            std::scoped_lock lock{synchronizationMutex};
            if (__atomic_load_n(mutex, __ATOMIC_SEQ_CST) != (ownerHandle | HandleWaitersBit))
                return failOnOutdated ? result::InvalidCurrentMemory : Result{};

            std::shared_ptr<KThread> owner;
            try {
                owner = GetHandle<KThread>(ownerHandle);
            } catch (const std::out_of_range &) {
                return result::InvalidHandle;
            }
            thread->waitThread = owner;
            thread->waitMutex = mutex;
            thread->waitTag = tag;
            thread->waitSignalled = false;
            thread->waitResult = {};
            owner->waiters.push_back(thread);
            if (thread == state.thread)
                state.scheduler->RemoveThread();
            thread->UpdatePriorityInheritance();
        }
        if (thread == state.thread)
            state.scheduler->WaitSchedule();
        return {};
    }

    void KProcess::MutexUnlock(u32 *mutex) {
        TRACE_EVENT_FMT("kernel", "MutexUnlock {}", fmt::ptr(mutex));
        std::scoped_lock lock{synchronizationMutex};
        auto owner{state.thread};
        auto &waiters{owner->waiters};
        waiters.sort([](const auto &a, const auto &b) { return a->priority < b->priority; });
        auto next{std::find_if(waiters.begin(), waiters.end(), [mutex](const auto &thread) { return thread->waitMutex == mutex; })};
        if (next == waiters.end()) {
            __atomic_store_n(mutex, 0, __ATOMIC_SEQ_CST);
            return;
        }

        auto nextOwner{*next};
        waiters.erase(next);
        nextOwner->waitThread.reset();
        nextOwner->waitMutex = nullptr;
        bool hasWaiters{};
        for (auto it{waiters.begin()}; it != waiters.end();) {
            auto current{it++};
            if ((*current)->waitMutex == mutex) {
                (*current)->waitThread = nextOwner;
                nextOwner->waiters.splice(nextOwner->waiters.end(), waiters, current);
                hasWaiters = true;
            }
        }
        owner->UpdatePriorityInheritance();
        nextOwner->UpdatePriorityInheritance();
        __atomic_store_n(mutex, nextOwner->waitTag | (hasWaiters ? HandleWaitersBit : 0), __ATOMIC_SEQ_CST);
        nextOwner->waitResult = {};
        nextOwner->waitSignalled = true;
        state.scheduler->InsertThread(nextOwner);
    }

    void KProcess::RemoveThreadWaiter(const std::shared_ptr<KThread> &thread) {
        std::scoped_lock lock{synchronizationMutex};
        for (auto *queue : {&conditionVariableWaiters, &addressWaiters}) {
            for (auto it{queue->begin()}; it != queue->end();) {
                if (it->second == thread)
                    it = queue->erase(it);
                else
                    ++it;
            }
        }
        if (auto owner{thread->waitThread}) {
            owner->waiters.remove(thread);
            thread->waitThread.reset();
            owner->UpdatePriorityInheritance();
        }
        thread->waitMutex = nullptr;
        thread->waitConditionVariable = nullptr;
    }

    Result KProcess::ConditionVariableWait(u32 *key, u32 *mutex, KHandle tag, i64 timeout) {
        TRACE_EVENT_FMT("kernel", "ConditionVariableWait {} ({})", fmt::ptr(key), fmt::ptr(mutex));
        auto thread{state.thread};
        {
            std::scoped_lock lock{synchronizationMutex};
            thread->waitThread.reset();
            thread->waitMutex = mutex;
            thread->waitTag = tag;
            thread->waitConditionVariable = key;
            thread->waitSignalled = false;
            thread->waitResult = {};
            __atomic_store_n(key, true, __ATOMIC_SEQ_CST);
            MutexUnlock(mutex);
            if (timeout == 0) {
                thread->waitMutex = nullptr;
                thread->waitConditionVariable = nullptr;
                return result::TimedOut;
            }
            conditionVariableWaiters.emplace(key, thread);
            state.scheduler->RemoveThread();
        }
        if (timeout > 0 && !state.scheduler->TimedWaitSchedule(std::chrono::nanoseconds(timeout))) {
            std::scoped_lock lock{synchronizationMutex};
            if (!thread->waitSignalled) {
                RemoveThreadWaiter(thread);
                thread->waitResult = result::TimedOut;
                thread->waitSignalled = true;
                state.scheduler->InsertThread(thread);
            }
        }
        state.scheduler->WaitSchedule(false);
        std::scoped_lock lock{synchronizationMutex};
        return thread->waitResult;
    }

    void KProcess::ConditionVariableSignal(u32 *key, i32 amount) {
        TRACE_EVENT_FMT("kernel", "ConditionVariableSignal {}", fmt::ptr(key));
        std::scoped_lock lock{synchronizationMutex};
        for (i32 signalled{}; amount <= 0 || signalled < amount; ++signalled) {
            auto queue{conditionVariableWaiters.equal_range(key)};
            if (queue.first == queue.second)
                break;
            auto it{std::min_element(queue.first, queue.second, [](const auto &a, const auto &b) { return a.second->priority < b.second->priority; })};
            auto thread{it->second};
            conditionVariableWaiters.erase(it);
            thread->waitConditionVariable = nullptr;
            auto mutex{thread->waitMutex};
            auto tag{thread->waitTag};
            while (true) {
                KHandle value{};
                if (__atomic_compare_exchange_n(mutex, &value, tag, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    thread->waitMutex = nullptr;
                    thread->waitResult = {};
                    thread->waitSignalled = true;
                    state.scheduler->InsertThread(thread);
                    break;
                }
                if (!(value & HandleWaitersBit) && !__atomic_compare_exchange_n(mutex, &value, value | HandleWaitersBit, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    continue;
                auto lockResult{MutexLock(thread, mutex, value & ~HandleWaitersBit, tag, true)};
                if (lockResult == result::InvalidCurrentMemory)
                    continue;
                if (lockResult == result::InvalidHandle) {
                    thread->waitMutex = nullptr;
                    thread->waitResult = result::InvalidState;
                    thread->waitSignalled = true;
                    state.scheduler->InsertThread(thread);
                } else if (lockResult != Result{}) {
                    throw exception("Failed to lock mutex: 0x{:X}", lockResult);
                }
                break;
            }
        }
        if (conditionVariableWaiters.count(key) == 0)
            __atomic_store_n(key, false, __ATOMIC_SEQ_CST);
    }

    Result KProcess::WaitForAddress(u32 *address, u32 value, i64 timeout, ArbitrationType type) {
        TRACE_EVENT_FMT("kernel", "WaitForAddress {}", fmt::ptr(address));

        {
            std::scoped_lock lock{synchronizationMutex};

            u32 userValue{__atomic_load_n(address, __ATOMIC_SEQ_CST)};
            switch (type) {
                case ArbitrationType::WaitIfLessThan:
                    if (!AddressIsLessThan(userValue, value)) [[unlikely]]
                        return result::InvalidState;
                    break;

                case ArbitrationType::DecrementAndWaitIfLessThan: {
                    do {
                        if (!AddressIsLessThan(userValue, value)) [[unlikely]] // We want to explicitly decrement **after** the check
                            return result::InvalidState;
                    } while (!__atomic_compare_exchange_n(address, &userValue, userValue - 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
                    break;
                }

                case ArbitrationType::WaitIfEqual:
                    if (userValue != value) [[unlikely]]
                        return result::InvalidState;
                    break;
            }

            if (timeout == 0) [[unlikely]]
                return result::TimedOut;

            auto queue{addressWaiters.equal_range(address)};
            addressWaiters.insert(std::upper_bound(queue.first, queue.second, state.thread->priority.load(), [](const i8 priority, const SyncWaiters::value_type &it) { return it.second->priority > priority; }), {address, state.thread});

            state.scheduler->RemoveThread();
        }

        if (timeout > 0 && !state.scheduler->TimedWaitSchedule(std::chrono::nanoseconds(timeout))) {
            bool shouldWait{false};
            {
                std::scoped_lock lock{synchronizationMutex};
                auto queue{addressWaiters.equal_range(address)};
                auto iterator{std::find(queue.first, queue.second, SyncWaiters::value_type{address, state.thread})};
                if (iterator != queue.second) {
                    addressWaiters.erase(iterator); // An arbiter word is not a condvar waiter flag.
                } else {
                    // If we didn't find the thread in the queue then it must have been signalled already and we should just wait
                    shouldWait = true;
                }
            }

            if (shouldWait) {
                state.scheduler->WaitSchedule(false);
                return {};
            }

            state.scheduler->InsertThread(state.thread);
            state.scheduler->WaitSchedule(false);

            return result::TimedOut;
        } else {
            state.scheduler->WaitSchedule(false);
        }

        return {};
    }

    Result KProcess::SignalToAddress(u32 *address, u32 value, i32 amount, SignalType type) {
        TRACE_EVENT_FMT("kernel", "SignalToAddress {}", fmt::ptr(address));

        std::scoped_lock lock{synchronizationMutex};
        auto queue{addressWaiters.equal_range(address)};

        if (type != SignalType::Signal) {
            u32 newValue{value};
            if (type == SignalType::SignalAndIncrementIfEqual) {
                newValue++;
            } else if (type == SignalType::SignalAndModifyBasedOnWaitingThreadCountIfEqual) {
                newValue = ModifyAddressByWaiterCount(value, amount, std::distance(queue.first, queue.second));
            }

            if (!__atomic_compare_exchange_n(address, &value, newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) [[unlikely]]
                return result::InvalidState;
        }

        i32 waiterCount{amount};
        boost::container::small_vector<SyncWaiters::iterator, 10> orderedThreads;
        for (auto it{queue.first}; it != queue.second; it++) {
            // While threads should generally be inserted in priority order, they may not always be due to the way the kernel handles priority updates
            // As a result, we need to create a second list of threads ordered by priority to ensure that we wake up the highest priority threads first
            auto thread{it->second};
            orderedThreads.insert(std::upper_bound(orderedThreads.begin(), orderedThreads.end(), thread->priority.load(), [](const i8 priority, const SyncWaiters::iterator &it) { return it->second->priority > priority; }), it);
        }

        for (auto &it : orderedThreads) {
            auto thread{it->second};

            addressWaiters.erase(it);
            state.scheduler->InsertThread(thread);

            if (--waiterCount == 0 && amount > 0)
                break;
        }

        return {};
    }
}
