// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <memory>
#include <atomic>

namespace skyline {
    /**
     * @brief A wrapper around a shared_ptr<T> which can be utilized to perform transactional atomic operations to lock the underlying resource and attain stability in the pointer value
     * @note Any operations directly accessing the value are **NOT** atomic and should be done after a locking transaction
     */
    template<typename Type>
    class LockableSharedPtr : public std::shared_ptr<Type> {
      public:
        using std::shared_ptr<Type>::shared_ptr;
        using std::shared_ptr<Type>::operator=;

        LockableSharedPtr(std::shared_ptr<Type> &&ptr) : std::shared_ptr<Type>{std::move(ptr)} {}

      private:
        /**
         * @brief A lock function for the underlying object that conforms to the BasicLockable named requirement
         */
        static void DefaultLockFunction(Type *object) {
            object->lock();
        }

        /**
         * @brief An unlock function for the underlying object that conforms to the BasicLockable named requirement
         */
        static void DefaultUnlockFunction(Type *object) {
            object->unlock();
        }

        /**
         * @brief A try_lock function for the underlying object that conforms to the Lockable named requirement
         */
        static bool DefaultTryLockFunction(Type *object) {
            return object->try_lock();
        }

      public:
        std::shared_ptr<Type> Load() const noexcept {
            return std::atomic_load_explicit(static_cast<const std::shared_ptr<Type> *>(this), std::memory_order_acquire);
        }

        // Caller holds both old/new resources AND serializes all owners (GPU channel lock).
        void Store(std::shared_ptr<Type> value) noexcept {
            std::atomic_store_explicit(static_cast<std::shared_ptr<Type> *>(this), std::move(value), std::memory_order_release);
        }

        /**
         * @brief Locks the underlying object with the supplied lock/unlock functions
         */
        template<typename LockFunction = decltype(DefaultLockFunction), typename UnlockFunction = decltype(DefaultUnlockFunction)>
        void Lock(LockFunction lock = DefaultLockFunction, UnlockFunction unlock = DefaultUnlockFunction) const {
            while (true) {
                auto object{this->get()};
                lock(object);

                if (this->get() == object)
                    return;

                unlock(object);
            }
        }

        /**
         * @brief Attempts to lock the underlying object with the supplied try_lock/unlock functions
         */
        template<typename TryLockFunction = decltype(DefaultTryLockFunction), typename UnlockFunction = decltype(DefaultUnlockFunction)>
        bool TryLock(TryLockFunction tryLock = DefaultTryLockFunction, UnlockFunction unlock = DefaultUnlockFunction) const {
            while (true) {
                auto object{this->get()};
                bool wasLocked{tryLock(object)};

                if (this->get() == object)
                    return wasLocked;

                if (wasLocked)
                    unlock(object);
            }
        }
    };
}
