// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "KSharedMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KSharedMemory::KSharedMemory(const DeviceState &state, size_t size)
        : KMemory{state, KType::KSharedMemory, size} {}

    u8 *KSharedMemory::Map(span<u8> map, memory::Permission permission) {
        return MapPrepared(map, permission, false);
    }

    u8 *KSharedMemory::MapPrepared(span<u8> map, memory::Permission permission, bool dynamicBacking) {
        dynamicGuestBacking = dynamicBacking;

        try {
            u8 *result{dynamicGuestBacking ? MapImpl(map, permission, true) : KMemory::Map(map, permission)};
            state.process->memory.MapSharedMemory(guest, permission);
            return result;
        } catch (...) {
            if (dynamicGuestBacking)
                state.process->memory.ReleaseSharedMemoryBacking36Bit(map);
            dynamicGuestBacking = false;
            throw;
        }
    }

    void KSharedMemory::Unmap(span<u8> map) {
        if (dynamicGuestBacking)
            UnmapImpl(map, true);
        else
            KMemory::Unmap(map);

        if (!state.process->memory.UnmapMemory(map)) [[unlikely]] {
            LOGW("KSharedMemory::Unmap: memory range is still IPC-locked: {} - {}", fmt::ptr(map.data()), fmt::ptr(map.end().base()));
            return;
        }

        if (dynamicGuestBacking)
            state.process->memory.ReleaseSharedMemoryBacking36Bit(map);

        dynamicGuestBacking = false;
        guest = span<u8>{};
    }

    KSharedMemory::~KSharedMemory() {
        if (state.process && guest.valid()) {
            auto mappedGuest{guest};
            auto hostMap{state.process->memory.GetHostSpan(mappedGuest)};
            if (mmap(hostMap.data(), hostMap.size(), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) [[unlikely]]
                LOGW("An error occurred while unmapping shared memory: {}", strerror(errno));

            if (state.process->memory.UnmapMemory(mappedGuest) && dynamicGuestBacking)
                state.process->memory.ReleaseSharedMemoryBacking36Bit(mappedGuest);
        }
    }
}
