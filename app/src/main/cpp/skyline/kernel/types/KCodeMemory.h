// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

#pragma once

#include <kernel/memory.h>
#include "KObject.h"

namespace skyline::kernel::type {
    /**
     * @brief Tracks a source range locked for code memory and its shared backing.
     *
     * Mapping the code into another address/process is deliberately not implemented
     * by this experimental class yet. This object is only created when the source
     * range can actually be locked and backed by a valid shared-memory descriptor.
     */
    class KCodeMemory : public KObject {
      private:
        span<u8> source;
        int backingFd{-1};
        bool sourceLocked{};

      public:
        KCodeMemory(const DeviceState &state, span<u8> source);
        Result Initialize();

        span<u8> GetSource() const { return source; }
        int GetBackingFd() const { return backingFd; }

        ~KCodeMemory();
    };
}
