// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

#pragma once

#include <kernel/memory.h>
#include "KObject.h"

namespace skyline::kernel::type {
    class KProcess;
    /** Borrowed physical pages shared by a RW compiler alias and an R/RX owner alias. */
    class KCodeMemory : public KObject {
      private:
        span<u8> source;
        int backingFd{-1};
        bool sourceLocked{};
        span<u8> writableBacking;
        u64 ownerAddress{}, writerAddress{};
        std::mutex mutex;
        std::weak_ptr<KProcess> owner;

        void SynchronizeLocked(size_t offset, size_t size);
        Result MapImpl(u64 address, size_t size, memory::Permission permission, bool toOwner, u64 *allocated);

      public:
        KCodeMemory(const DeviceState &state, span<u8> source);
        Result Initialize();

        span<u8> GetSource() const { return source; }
        int GetBackingFd() const { return backingFd; }

        span<u8> GetWritableBacking() const { return writableBacking; }
        Result Map(u64 address, size_t size, memory::Permission permission, bool toOwner);
        Result MapToOwner(size_t size, memory::Permission permission, u64 &address);
        Result MapAnywhere(size_t size, memory::Permission permission, u64 &address);
        Result Unmap(u64 address, size_t size, bool fromOwner);
        void Synchronize(size_t offset, size_t size);
        ~KCodeMemory();
    };
}
