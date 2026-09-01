// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "KTransferMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KTransferMemory::KTransferMemory(const DeviceState &state, size_t size)
        : KMemory{state, KType::KTransferMemory, size} {}

    void KTransferMemory::RestoreOriginalMapping(span<u8> map) {
        switch (originalMapping.state.type) {
            case memory::MemoryType::Code:
                state.process->memory.MapCodeMemory(map, originalMapping.permission);
                break;
            case memory::MemoryType::CodeMutable:
                state.process->memory.MapMutableCodeMemory(map);
                break;
            case memory::MemoryType::Stack:
                state.process->memory.MapStackMemory(map);
                break;
            case memory::MemoryType::Heap:
                state.process->memory.MapHeapMemory(map);
                break;
            case memory::MemoryType::SharedMemory:
                state.process->memory.MapSharedMemory(map, originalMapping.permission);
                break;
            case memory::MemoryType::TransferMemory:
            case memory::MemoryType::TransferMemoryIsolated:
                state.process->memory.MapTransferMemory(map, originalMapping.permission);
                break;
            case memory::MemoryType::ThreadLocal:
                state.process->memory.MapThreadLocalMemory(map);
                break;
            case memory::MemoryType::Reserved:
                state.process->memory.Reserve(map);
                break;
            case memory::MemoryType::Unmapped:
                if (!state.process->memory.UnmapMemory(map)) [[unlikely]]
                    LOGW("KTransferMemory: UnmapMemory rejected while restoring Unmapped state at: {} (0x{:X} bytes)", fmt::ptr(map.data()), map.size());
                break;
            default:
                LOGW("Restoring KTransferMemory with incompatible state: (0x{:X})", originalMapping.state.value);
                if (!state.process->memory.UnmapMemory(map)) [[unlikely]]
                    LOGW("KTransferMemory: UnmapMemory rejected in fail-safe restore at: {} (0x{:X} bytes)", fmt::ptr(map.data()), map.size());
        }
    }

    u8 *KTransferMemory::Map(span<u8> map, memory::Permission permission) {
        auto oldChunk{state.process->memory.GetChunk(map.data()).value()};
        originalMapping = oldChunk.second;

        if (!originalMapping.state.transferMemoryAllowed) [[unlikely]] {
            LOGW("Tried to map transfer memory with incompatible state at: {} (0x{:X} bytes, state: 0x{:X}, type: 0x{:X})", fmt::ptr(map.data()), map.size(), originalMapping.state.value, static_cast<u32>(originalMapping.state.type));
            return nullptr;
        }

        // Keep a host-visible snapshot for existing HLE users of KTransferMemory::host.
        auto hostMap{state.process->memory.GetHostSpan(map)};
        std::memcpy(host.data(), hostMap.data(), hostMap.size());

        if (!permission.raw) {
            // Horizon's LockForTransferMemory keeps the owner's original physical pages and
            // changes their owner permission/Locked attribute. Do the same for the common
            // permission=None path instead of MAP_FIXED-replacing the region with a copied
            // ASharedMemory object. Newer nnSdk audio clients validate this ownership model.
            if (mprotect(hostMap.data(), hostMap.size(), PROT_NONE) == -1) [[unlikely]] {
                LOGW("Failed to protect TransferMemory owner backing at {} (0x{:X} bytes): {}", fmt::ptr(map.data()), map.size(), strerror(errno));
                return nullptr;
            }

            guest = map;
            ownerBackingPreserved = true;
        } else {
            if (!KMemory::Map(map, permission)) [[unlikely]]
                return nullptr;
        }

        // The owner's memory type stays unchanged; only permission and Locked/Borrowed change.
        state.process->memory.SetRegionPermission(guest, permission);
        state.process->memory.SetRegionBorrowed(guest, true);
        return guest.data();
    }

    void KTransferMemory::Unmap(span<u8> map) {
        if (ownerBackingPreserved) {
            auto hostMap{state.process->memory.GetHostSpan(map)};

            // Temporarily make the original pages writable so HLE-side changes to the
            // transfer-memory snapshot can be committed before restoring owner protection.
            if (mprotect(hostMap.data(), hostMap.size(), PROT_READ | PROT_WRITE) == -1) [[unlikely]]
                throw exception("Failed to unprotect TransferMemory owner backing: {}", strerror(errno));

            std::memcpy(hostMap.data(), host.data(), hostMap.size());

            const int originalProtection{originalMapping.permission.Get()};
            if (mprotect(hostMap.data(), hostMap.size(), originalProtection) == -1) [[unlikely]]
                throw exception("Failed to restore TransferMemory owner protection: {}", strerror(errno));
        } else {
            KMemory::Unmap(map);
            auto hostMap{state.process->memory.GetHostSpan(map)};
            std::memcpy(hostMap.data(), host.data(), hostMap.size());
        }

        RestoreOriginalMapping(map);
        guest = span<u8>{};
        ownerBackingPreserved = false;
    }

    KTransferMemory::~KTransferMemory() {
        if (!state.process || !guest.valid())
            return;

        if (ownerBackingPreserved) {
            auto hostMap{state.process->memory.GetHostSpan(guest)};

            if (mprotect(hostMap.data(), hostMap.size(), PROT_READ | PROT_WRITE) == -1) [[unlikely]] {
                LOGW("Failed to unprotect TransferMemory owner backing during destruction: {}", strerror(errno));
            } else {
                std::memcpy(hostMap.data(), host.data(), hostMap.size());

                if (mprotect(hostMap.data(), hostMap.size(), originalMapping.permission.Get()) == -1) [[unlikely]]
                    LOGW("Failed to restore TransferMemory owner protection during destruction: {}", strerror(errno));
            }
        } else {
            if (mmap(state.process->memory.GetHostSpan(guest).data(), guest.size(), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) [[unlikely]]
                LOGW("An error occurred while unmapping transfer memory in guest: {}", strerror(errno));

            auto hostMap{state.process->memory.GetHostSpan(guest)};
            std::memcpy(hostMap.data(), host.data(), hostMap.size());
        }

        RestoreOriginalMapping(guest);
    }
}
