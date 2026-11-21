// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common/trace.h>
#include <common/spin_lock.h>
#include <common/span.h>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace skyline {
    /**
     * @brief A bounded producer-consumer ring with object lifetime management.
     *
     * Slots hold aligned storage rather than already-constructed objects. An
     * item is constructed when published, then destroyed after consumption;
     * this is important for frames containing shared_ptr and std::function.
     */
    template<typename Type>
    class CircularQueue {
      private:
        struct Slot {
            alignas(Type) std::byte storage[sizeof(Type)];
            Type *Get() { return std::launder(reinterpret_cast<Type *>(storage)); }
            Type *Uninitialized() { return reinterpret_cast<Type *>(storage); }
        };

        std::vector<Slot> slots; //!< One sentinel slot disambiguates full/empty
        std::atomic<size_t> start{}; //!< Index of the last consumed item
        std::atomic<size_t> end{}; //!< Index of the last published item
        SpinLock consumptionMutex;
        std::condition_variable_any consumeCondition;
        SpinLock productionMutex;
        std::condition_variable_any produceCondition;
        std::atomic_bool stopped{false};

        size_t Next(size_t index) const {
            return ++index == slots.size() ? 0 : index;
        }

      public:
        explicit CircularQueue(size_t size) : slots(size + 1) {}

        CircularQueue(const CircularQueue &) = delete;
        CircularQueue &operator=(const CircularQueue &) = delete;

        // A moved queue must not have concurrent producers or consumers.
        CircularQueue(CircularQueue &&other)
            : slots(std::move(other.slots)),
              start(other.start.load(std::memory_order_relaxed)),
              end(other.end.load(std::memory_order_relaxed)),
              stopped(other.stopped.load(std::memory_order_relaxed)) {
            other.start.store(0, std::memory_order_relaxed);
            other.end.store(0, std::memory_order_relaxed);
        }

        ~CircularQueue() {
            auto current{start.load(std::memory_order_relaxed)};
            const auto last{end.load(std::memory_order_relaxed)};
            while (current != last) {
                current = Next(current);
                std::destroy_at(slots[current].Get());
            }
        }

        bool Empty() const {
            return start.load(std::memory_order_acquire) == end.load(std::memory_order_acquire);
        }

        /**
         * @brief Stop accepting new items and let Process drain already queued work.
         */
        void Close() {
            {
                std::scoped_lock lock{productionMutex};
                stopped.store(true, std::memory_order_release);
            }
            produceCondition.notify_all();
            consumeCondition.notify_all();
        }

        template<typename F1, typename F2>
        void Process(F1 function, F2 preWait) {
            TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");
            while (true) {
                if (Empty()) {
                    std::unique_lock productionLock{productionMutex};
                    TRACE_EVENT_END("containers");
                    preWait();
                    produceCondition.wait(productionLock, [this]() {
                        return !Empty() || stopped.load(std::memory_order_acquire);
                    });
                    TRACE_EVENT_BEGIN("containers", "CircularQueue::Process");
                    if (Empty()) {
                        TRACE_EVENT_END("containers");
                        return;
                    }
                }

                std::scoped_lock consumptionLock{consumptionMutex};
                while (!Empty()) {
                    auto next{Next(start.load(std::memory_order_relaxed))};
                    auto *item{slots[next].Get()};
                    function(*item);
                    std::destroy_at(item);
                    start.store(next, std::memory_order_release);
                }
                consumeCondition.notify_all();
            }
        }

        Type Pop() {
            {
                std::unique_lock productionLock{productionMutex};
                produceCondition.wait(productionLock, [this]() {
                    return !Empty() || stopped.load(std::memory_order_acquire);
                });
                if (Empty())
                    throw std::runtime_error("CircularQueue closed");
            }

            std::scoped_lock consumptionLock{consumptionMutex};
            auto next{Next(start.load(std::memory_order_relaxed))};
            Type item{std::move(*slots[next].Get())};
            std::destroy_at(slots[next].Get());
            start.store(next, std::memory_order_release);
            consumeCondition.notify_all();
            return item;
        }

        void Push(const Type &item) {
            Emplace(item);
        }

        void Push(Type &&item) {
            Emplace(std::move(item));
        }

        template<typename... Args>
        void Emplace(Args &&... args) {
            size_t fullNext{};
            size_t fullEnd{};
            bool wasFull{};

            while (true) {
                if (wasFull) {
                    std::unique_lock consumptionLock{consumptionMutex};
                    consumeCondition.wait(consumptionLock, [=, this]() {
                        return stopped.load(std::memory_order_acquire) ||
                               fullNext != start.load(std::memory_order_acquire) ||
                               fullEnd != end.load(std::memory_order_acquire);
                    });
                    wasFull = false;
                }

                std::scoped_lock productionLock{productionMutex};
                if (stopped.load(std::memory_order_acquire))
                    throw std::runtime_error("CircularQueue closed");
                const auto currentEnd{end.load(std::memory_order_relaxed)};
                const auto next{Next(currentEnd)};
                if (next == start.load(std::memory_order_acquire)) {
                    fullNext = next;
                    fullEnd = currentEnd;
                    wasFull = true;
                    continue;
                }
                std::construct_at(slots[next].Uninitialized(), std::forward<Args>(args)...);
                end.store(next, std::memory_order_release);
                produceCondition.notify_one();
                break;
            }
        }

        template<typename T>
        void Append(span<T> buffer) {
            for (const auto &item : buffer)
                Push(item);
        }

        template<typename TransformedType, typename Transformation>
        void AppendTranform(TransformedType &container, Transformation transformation) {
            for (const auto &item : container)
                Push(transformation(item));
        }
    };
}
