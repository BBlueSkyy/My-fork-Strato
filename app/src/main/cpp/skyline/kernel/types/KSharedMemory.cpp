// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "KSharedMemory.h"
#include "KProcess.h"
#include <input.h>

namespace skyline::kernel::type {
    KSharedMemory::KSharedMemory(const DeviceState &state, size_t size)
        : KMemory{state, KType::KSharedMemory, size} {}

    u8 *KSharedMemory::Map(span<u8> map, memory::Permission permission) {
        u8 *result{KMemory::Map(map, permission)};

        state.process->memory.MapSharedMemory(guest, permission);

        if (state.input && state.input->kHid.get() == this && map.size() == sizeof(input::HidSharedMemory)) {
            constexpr std::array<size_t, 2> offsets{
                offsetof(input::HidSharedMemory, npad) + offsetof(input::NpadSection, fullKeyController),
                offsetof(input::HidSharedMemory, npad) + offsetof(input::NpadSection, defaultController) +
                    offsetof(input::NpadControllerInfo, state) + 11 * sizeof(input::NpadControllerState),
            };
            for (size_t index{}; index < offsets.size(); ++index) {
                span<u8> page{util::AlignDown(result + offsets[index], constant::PageSize), constant::PageSize};
                npadReadProbes[index] = state.process->trap.CreateTrap(page, {}, [] { return true; }, [] { return true; });
                state.process->trap.TrapRegions(*npadReadProbes[index], false);
            }
        }

        return result;
    }

    void KSharedMemory::Unmap(span<u8> map) {
        for (auto &probe : npadReadProbes) {
            if (probe) {
                state.process->trap.DeleteTrap(*probe);
                probe.reset();
            }
        }
        KMemory::Unmap(map);

        guest = span<u8>{};
        state.process->memory.UnmapMemory(map);
    }

    KSharedMemory::~KSharedMemory() {
        for (auto &probe : npadReadProbes)
            if (probe && state.process)
                state.process->trap.DeleteTrap(*probe);
        if (state.process && guest.valid()) {
            auto hostMap{state.process->memory.GetHostSpan(guest)};
            if (mmap(hostMap.data(), hostMap.size(), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) [[unlikely]]
                LOGW("An error occurred while unmapping shared memory: {}", strerror(errno));

            state.process->memory.UnmapMemory(guest);
        }
    }
}
