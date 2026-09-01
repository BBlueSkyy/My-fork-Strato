// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "KTransferMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KTransferMemory::KTransferMemory(const DeviceState &state, size_t size)
        : KMemory{state, KType::KTransferMemory, size} {}

    u8 *KTransferMemory::Map(span<u8> map, memory::Permission permission) {
        // FIX: validate the original chunk's state BEFORE doing anything destructive.
        // Previously this check ran *after* KMemory::Map() had already remapped the
        // guest's physical backing, so a failed check (!transferMemoryAllowed) still
        // left the memory physically read/write-able while MapTransferMemory() (which
        // updates the MemoryManager's chunk tracking) was skipped on the early return.
        // That desync is exactly what produced "state 0x0 (type: 0x0)" rejections
        // later on for memory that the guest could actually read/write fine.
        auto oldChunk{state.process->memory.GetChunk(map.data()).value()};
        originalMapping = oldChunk.second;

        if (!originalMapping.state.transferMemoryAllowed) [[unlikely]] {
            LOGW("Tried to map transfer memory with incompatible state at: {} (0x{:X} bytes, state: 0x{:X}, type: 0x{:X})", fmt::ptr(map.data()), map.size(), originalMapping.state.value, static_cast<u32>(originalMapping.state.type));
            return nullptr; // Nothing has been touched yet, safe to bail out here
        }

        // HOS 19.0.0+ exposes InfoType_TransferMemoryHint (34), which is based on the
        // owner's original source VA. Keep that address even though KMemory::Map later
        // replaces the owner mapping with the shared-memory-backed guest span.
        sourceAddress = reinterpret_cast<uintptr_t>(map.data());

        // Get the host address of the guest memory
        auto hostMap{state.process->memory.GetHostSpan(map)};
        std::memcpy(host.data(), hostMap.data(), hostMap.size());
        u8 *result{KMemory::Map(map, permission)};

        // svcCreateTransferMemory locks the owner's existing region; it does not turn
        // that owner-side mapping into MemoryType::TransferMemory/TransferMemoryIsolated.
        // Preserve the original MemoryState and only apply the owner permission plus the
        // Locked/Borrowed attribute. This matches Horizon's LockForTransferMemory semantics
        // and keeps svcQueryMemory reporting the original type (normally Heap).
        state.process->memory.SetRegionPermission(guest, permission);
        state.process->memory.SetRegionBorrowed(guest, true);
        return result;
    }

    uintptr_t KTransferMemory::GetHint() const {
        const size_t size{host.size()};

        // Matches Horizon/Mesosphere KTransferMemory::GetHint(). The kernel exposes
        // the low address bits corresponding to the largest placement granularity
        // applicable to the transfer-memory size.
        if (size >= 0x200000)
            return sourceAddress & (0x200000 - 1);
        if (size >= 0x10000)
            return sourceAddress & (0x10000 - 1);
        if (size >= 0x1000)
            return sourceAddress & (0x1000 - 1);
        return 0;
    }

    void KTransferMemory::Unmap(span<u8> map) {
        KMemory::Unmap(map);

        guest = span<u8>{};
        // FIX: restore *every* memory type the transfer memory could have been carved
        // out of, not just CodeMutable/Heap. Previously anything else (Stack,
        // SharedMemory, TransferMemory, ThreadLocal, Reserved, or Unmapped) fell into
        // `default` and only logged a warning, leaving the MemoryManager's chunk
        // tracking permanently out of sync with the physically-restored host memory.
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
                    LOGW("KTransferMemory::Unmap: UnmapMemory rejected (IPC-locked?) restoring Unmapped state at: {} (0x{:X} bytes) - tracking may be desynced", fmt::ptr(map.data()), map.size());
                break;
            default:
                LOGW("Unmapping KTransferMemory with incompatible state: (0x{:X})", originalMapping.state.value);
                if (!state.process->memory.UnmapMemory(map)) [[unlikely]] // Fail safe rather than leaving stale tracking behind
                    LOGW("KTransferMemory::Unmap: UnmapMemory rejected (IPC-locked?) in fail-safe path at: {} (0x{:X} bytes) - tracking may be desynced", fmt::ptr(map.data()), map.size());
        }
        map = state.process->memory.GetHostSpan(map);
        std::memcpy(map.data(), host.data(), map.size());
    }

    KTransferMemory::~KTransferMemory() {
        if (state.process && guest.valid()) {
            if (mmap(guest.data(), guest.size(), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) [[unlikely]]
                LOGW("An error occurred while unmapping transfer memory in guest: {}", strerror(errno));

            // FIX: same exhaustive restore as Unmap() above, see comment there.
            switch (originalMapping.state.type) {
                case memory::MemoryType::Code:
                    state.process->memory.MapCodeMemory(guest, originalMapping.permission);
                    break;
                case memory::MemoryType::CodeMutable:
                    state.process->memory.MapMutableCodeMemory(guest);
                    break;
                case memory::MemoryType::Stack:
                    state.process->memory.MapStackMemory(guest);
                    break;
                case memory::MemoryType::Heap:
                    state.process->memory.MapHeapMemory(guest);
                    break;
                case memory::MemoryType::SharedMemory:
                    state.process->memory.MapSharedMemory(guest, originalMapping.permission);
                    break;
                case memory::MemoryType::TransferMemory:
                case memory::MemoryType::TransferMemoryIsolated:
                    state.process->memory.MapTransferMemory(guest, originalMapping.permission);
                    break;
                case memory::MemoryType::ThreadLocal:
                    state.process->memory.MapThreadLocalMemory(guest);
                    break;
                case memory::MemoryType::Reserved:
                    state.process->memory.Reserve(guest);
                    break;
                case memory::MemoryType::Unmapped:
                    if (!state.process->memory.UnmapMemory(guest)) [[unlikely]]
                        LOGW("~KTransferMemory: UnmapMemory rejected (IPC-locked?) restoring Unmapped state at: {} (0x{:X} bytes) - tracking may be desynced", fmt::ptr(guest.data()), guest.size());
                    break;
                default:
                    LOGW("Unmapping KTransferMemory with incompatible state: (0x{:X})", originalMapping.state.value);
                    if (!state.process->memory.UnmapMemory(guest)) [[unlikely]] // Fail safe rather than leaving stale tracking behind
                        LOGW("~KTransferMemory: UnmapMemory rejected (IPC-locked?) in fail-safe path at: {} (0x{:X} bytes) - tracking may be desynced", fmt::ptr(guest.data()), guest.size());
            }
            std::memcpy(guest.data(), host.data(), guest.size());
        }
    }
}
