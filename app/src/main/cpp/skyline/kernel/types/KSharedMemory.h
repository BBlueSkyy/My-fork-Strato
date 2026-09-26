// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <optional>
#include <common/trap_manager.h>
#include "KMemory.h"

namespace skyline::kernel::type {
    /**
     * @brief KSharedMemory is used to retain two mappings of the same underlying memory, allowing sharing memory between two processes
     */
    class KSharedMemory : public KMemory {
      private:
        // Temporary guest-read probes for the Npad diagnostic branch.
        std::array<std::optional<TrapHandle>, 2> npadReadProbes{};

      public:
        KSharedMemory(const DeviceState &state, size_t size);

        /**
         * @note 'ptr' needs to be in guest-reserved address space
         */
        u8 *Map(span<u8> map, memory::Permission permission);

        /**
         * @note 'ptr' needs to be in guest-reserved address space
         */
        void Unmap(span<u8> map);

        /**
         * @brief The destructor of shared memory, it deallocates the memory from all processes
         */
        ~KSharedMemory();
    };
}
