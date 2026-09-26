// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common/trace.h>
#include <common/spin_lock.h>
#include <common/span.h>
#include <logger/logger.h>
#include <condition_variable>
#include <mutex>
#include <type_traits>

namespace skyline {
    /**
     * @brief An efficient consumer-producer oriented queue with internal synchronization
     */
    template<typename Type, bool StandardProducerWait = false>
    class CircularQueue {
      private:
        using ProductionMutex = std::conditional_t<StandardProducerWait, std::mutex, SpinLock>;
        using ProduceCondition = std::conditional_t<StandardProducerWait, std::condition_variable, std::condition_variable_any>;
        std::vector<u8> vector; //!< The internal vector holding the circular queue's data, we use a byte vector due to the default item construction/destruction semantics not being appropriate for a circular buffer
        std::atomic<Type *> start{reinterpret_cast<Type *>(vector.begin().base())}; //!< The start/oldest element of the queue
        std::atomic<Type *> end{reinterpret_cast<Type *>(vector.begin().base())}; //!< The end/newest element of the queue
        SpinLock consumptionMutex;
        std::condition_variable_any consumeCondition;
        ProductionMutex productionMutex;
        ProduceCondition produceCondition;
        std::atomic_bool stopped{false}; //!< Set via Close() to cooperatively wake up a blocked Process() and let it return, without needing to interrupt it via a signal
        bool diagnosticTrace{}; //!< Enables temporary queue lifecycle logging for targeted diagnostics
        std::atomic<u64> diagnosticPredicateChecks{}; //!< Number of wait-predicate evaluations for the targeted diagnostic queue
        std::atomic_bool diagnosticLastReady{}; //!< Result of the most recent wait-predicate evaluation

      public:
        /**
         * @note The internal allocation is an item larger as we require a sentinel value
         */
        CircularQueue(size_t size, bool diagnosticTrace = false) :
            vector((size + 1) * sizeof(Type)),
            diagnosticTrace(diagnosticTrace) {}

        CircularQueue(const CircularQueue &) = delete;

        CircularQueue &operator=(const CircularQueue &) = delete;

        CircularQueue(CircularQueue &&other) :
            vector(std::move(other.vector)),
            diagnosticTrace(other.diagnosticTrace) {
            start = other.start;
            end = other.end;
            other.start = other.end = nullptr;
        }

        ~CircularQueue() {
            while (start != end) {
                auto next{start + 1};
                next = (next == reinterpret_cast<Type *>(vector.end().base())) ? reinterpret_cast<Type *>(vector.begin().base()) : next;
                std::destroy_at(next);
                start = next;
            }
        }

        /**
         * @return Whether the queue is empty
         */
        bool Empty() {
            return start == end;
        }

        /**
         * @brief Cooperatively wakes up a call to Process() that's currently blocked waiting for more items and
         * lets it return once any items still queued have been processed
         * @note This exists so callers don't need to interrupt the waiting thread via a signal to stop it, which
         * is unreliable when the wait happens inside library code that doesn't preserve frame pointers
         */
        void Close() {
            {
                std::scoped_lock lock{productionMutex};
                stopped.store(true, std::memory_order_release);
            }
            produceCondition.notify_all();
        }

        /**
         * @brief A blocking for-each that runs on every item and waits till new items to run on them as well
         * @param function A function that is called for each item (with the only parameter as a reference to that item)
         * @param preWait An optional function that's called prior to waiting on more items to be queued
         * @note Returns once Close() has been called and there are no items left to process
         */
        template<typename F1, typename F2>
        void Process(F1 function, F2 preWait) {
            TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");

            while (true) {
                if (start == end) {
                    std::unique_lock productionLock{productionMutex};
                    TRACE_EVENT_END("containers");
                    if (diagnosticTrace)
                        LOGI("GRID-QUEUE pre-wait start={} end={} stopped={}",
                             static_cast<const void *>(start.load(std::memory_order_acquire)),
                             static_cast<const void *>(end.load(std::memory_order_acquire)),
                             stopped.load(std::memory_order_acquire));
                    preWait();
                    if (diagnosticTrace)
                        LOGI("GRID-QUEUE wait-enter start={} end={} stopped={}",
                             static_cast<const void *>(start.load(std::memory_order_acquire)),
                             static_cast<const void *>(end.load(std::memory_order_acquire)),
                             stopped.load(std::memory_order_acquire));
                    produceCondition.wait(productionLock, [this]() {
                        bool ready{start != end || stopped.load(std::memory_order_acquire)};
                        if (diagnosticTrace) {
                            u64 checks{diagnosticPredicateChecks.fetch_add(1, std::memory_order_relaxed) + 1};
                            diagnosticLastReady.store(ready, std::memory_order_relaxed);
                            LOGI("GRID-QUEUE predicate-check count={} ready={} start={} end={} stopped={}",
                                 checks, ready,
                                 static_cast<const void *>(start.load(std::memory_order_acquire)),
                                 static_cast<const void *>(end.load(std::memory_order_acquire)),
                                 stopped.load(std::memory_order_acquire));
                        }
                        return ready;
                    });
                    if (diagnosticTrace)
                        LOGI("GRID-QUEUE wait-return start={} end={} stopped={}",
                             static_cast<const void *>(start.load(std::memory_order_acquire)),
                             static_cast<const void *>(end.load(std::memory_order_acquire)),
                             stopped.load(std::memory_order_acquire));
                    TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");

                    if (start == end) {
                        // We were woken up by Close() and there's nothing left queued, time to stop
                        TRACE_EVENT_END("containers");
                        return;
                    }
                }

                if (diagnosticTrace)
                    LOGI("GRID-QUEUE consumption-lock-begin start={} end={}",
                         static_cast<const void *>(start.load(std::memory_order_acquire)),
                         static_cast<const void *>(end.load(std::memory_order_acquire)));
                std::scoped_lock comsumptionLock{consumptionMutex};
                if (diagnosticTrace)
                    LOGI("GRID-QUEUE consumption-lock-end start={} end={}",
                         static_cast<const void *>(start.load(std::memory_order_acquire)),
                         static_cast<const void *>(end.load(std::memory_order_acquire)));
                while (start != end) {
                    auto next{start + 1};
                    next = (next == reinterpret_cast<Type *>(vector.end().base())) ? reinterpret_cast<Type *>(vector.begin().base()) : next;
                    function(*next);
                    start = next;
                }

                consumeCondition.notify_one();
            }
        }

        Type Pop() {
            {
                std::unique_lock productionLock{productionMutex};
                produceCondition.wait(productionLock, [this]() { return start != end; });
            }

            std::scoped_lock comsumptionLock{consumptionMutex};
            auto next{start + 1};
            next = (next == reinterpret_cast<Type *>(vector.end().base())) ? reinterpret_cast<Type *>(vector.begin().base()) : next;
            Type item{std::move(*next)};
            start = next;

            consumeCondition.notify_one();

            return item;
        }

        void Push(const Type &item) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                auto next{end + 1};
                next = (next == reinterpret_cast<Type *>(vector.end().base())) ? reinterpret_cast<Type *>(vector.begin().base()) : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                *next = item;
                end = next;
                if (diagnosticTrace)
                    LOGI("GRID-QUEUE producer-notify-before checks={} lastReady={} start={} end={}",
                         diagnosticPredicateChecks.load(std::memory_order_relaxed),
                         diagnosticLastReady.load(std::memory_order_relaxed),
                         static_cast<const void *>(start.load(std::memory_order_acquire)),
                         static_cast<const void *>(end.load(std::memory_order_acquire)));
                produceCondition.notify_one();
                if (diagnosticTrace)
                    LOGI("GRID-QUEUE producer-notify-after checks={} lastReady={} start={} end={}",
                         diagnosticPredicateChecks.load(std::memory_order_relaxed),
                         diagnosticLastReady.load(std::memory_order_relaxed),
                         static_cast<const void *>(start.load(std::memory_order_acquire)),
                         static_cast<const void *>(end.load(std::memory_order_acquire)));
                break;
            }
        }

        void Push(Type &&item) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                auto next{end + 1};
                next = (next == reinterpret_cast<Type *>(vector.end().base())) ? reinterpret_cast<Type *>(vector.begin().base()) : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                *next = std::move(item);
                end = next;
                if (diagnosticTrace)
                    LOGI("GRID-QUEUE producer-notify-before checks={} lastReady={} start={} end={}",
                         diagnosticPredicateChecks.load(std::memory_order_relaxed),
                         diagnosticLastReady.load(std::memory_order_relaxed),
                         static_cast<const void *>(start.load(std::memory_order_acquire)),
                         static_cast<const void *>(end.load(std::memory_order_acquire)));
                produceCondition.notify_one();
                if (diagnosticTrace)
                    LOGI("GRID-QUEUE producer-notify-after checks={} lastReady={} start={} end={}",
                         diagnosticPredicateChecks.load(std::memory_order_relaxed),
                         diagnosticLastReady.load(std::memory_order_relaxed),
                         static_cast<const void *>(start.load(std::memory_order_acquire)),
                         static_cast<const void *>(end.load(std::memory_order_acquire)));
                break;
            }
        }

        template<typename... Args>
        void Emplace(Args &&... args) {
            Type *waitNext{};
            Type *waitEnd{};

            while (true) {
                if (waitNext) {
                    std::unique_lock consumeLock{consumptionMutex};

                    consumeCondition.wait(consumeLock, [=, this]() { return waitNext != start || waitEnd != end; });
                    waitNext = nullptr;
                    waitEnd = nullptr;
                }

                std::scoped_lock lock{productionMutex};
                auto next{end + 1};
                next = (next == reinterpret_cast<Type *>(vector.end().base())) ? reinterpret_cast<Type *>(vector.begin().base()) : next;
                if (next == start) {
                    waitNext = next;
                    waitEnd = end;
                    continue;
                }
                std::construct_at(next, std::forward<Args>(args)...);
                end = next;
                produceCondition.notify_one();
                break;
            }
        }

        /**
         * @note The appended elements may not necessarily be directly contiguous as another thread could push elements in between those in the span
         */
        void Append(span<Type> buffer) {
            for (const auto &item : buffer)
                Push(item);
        }

        /**
         * @brief Appends a buffer with an alternative input type while supplied transformation function
         * @param tranformation A function that takes in an item of TransformedType as input and returns an item of Type
         */
        template<typename TransformedType, typename Transformation>
        void AppendTranform(TransformedType &container, Transformation transformation) {
            for (const auto &item : container)
                Push(transformation(item));
        }
    };
}
