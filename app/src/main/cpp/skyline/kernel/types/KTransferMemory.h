// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "KMemory.h"

namespace skyline::kernel::type {
    /**
     * @brief KTransferMemory is used to transfer memory from one application to another on HOS, we emulate this abstraction using KSharedMemory as it's essentially the same with the main difference being that KSharedMemory is allocated by the kernel while KTransferMemory is created from memory that's been allocated by the guest beforehand
     * @note KSharedMemory::{Map, Unmap, ~KSharedMemory} contains code to handle differences in memory attributes and destruction
     */
    class KTransferMemory : public KMemory {
      private:
        ChunkDescriptor originalMapping;
        uintptr_t sourceAddress{}; //!< Original owner VA supplied to svcCreateTransferMemory; used by GetInfo(TransferMemoryHint)

      public:
        /**
         * @note 'ptr' needs to be in guest-reserved address space
         */
        KTransferMemory(const DeviceState &state, size_t size);

        /**
         * @note 'ptr' needs to be in guest-reserved address space
         */
        u8 *Map(span<u8> map, memory::Permission permission);

        /**
         * @brief Returns the HOS 19.0.0+ address-placement hint for this transfer memory.
         * @note The hint is derived from the original owner address and the transfer-memory size.
         */
        [[nodiscard]] uintptr_t GetHint() const;

        /**
         * @note 'ptr' needs to be in guest-reserved address space
         */
        void Unmap(span<u8> map);

        ~KTransferMemory();
    };
}
