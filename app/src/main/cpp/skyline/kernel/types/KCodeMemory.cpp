// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

#include <android/sharedmem.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <kernel/results.h>
#include "KCodeMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KCodeMemory::KCodeMemory(const DeviceState &state, span<u8> source)
        : KObject(state, KType::KCodeMemory), source{source}, owner{state.process} {}

    Result KCodeMemory::Initialize() {
        if (!state.process || !state.process->memory.AddressSpaceContains(source))
            return result::InvalidCurrentMemory;

        // Prepare the same physical backing for the source mapping and later
        // aliases. There is no second independent copy of the generated code.
        const int fd{ASharedMemory_create("HOS-KCodeMemory", source.size())};
        if (fd < 0) {
            LOGW("JIT_DIAG: ASharedMemory_create(0x{:X}) failed: {}", source.size(), strerror(errno));
            return result::OutOfMemory;
        }

        void *initMap{mmap(nullptr, source.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)};
        if (initMap == MAP_FAILED) {
            LOGW("JIT_DIAG: CodeMemory backing mmap failed: {}", strerror(errno));
            close(fd);
            return result::OutOfMemory;
        }

        // Horizon fills locked CodeMemory pages with 0xFF.
        std::memset(initMap, 0xFF, source.size());


        if (!state.process->memory.LockRegionForCodeMemory(source)) {
            LOGW("JIT_DIAG: CreateCodeMemory source validation/lock failed at {} (size=0x{:X})",
                 fmt::ptr(source.data()), source.size());
            munmap(initMap, source.size());
            close(fd);
            return result::InvalidCurrentMemory;
        }

        // Source and compiler/owner aliases all reference the same physical pages.
        auto hostSource{state.process->memory.GetHostSpan(source)};
        void *mapped{mmap(hostSource.data(), hostSource.size(), PROT_NONE,
                          MAP_FIXED | MAP_SHARED, fd, 0)};
        if (mapped == MAP_FAILED) {
            LOGW("JIT_DIAG: failed to replace locked source mapping: {}", strerror(errno));
            munmap(initMap, source.size());
            state.process->memory.UnlockRegionForCodeMemory(source);
            close(fd);
            return result::OutOfMemory;
        }

        writableBacking = span<u8>{static_cast<u8 *>(initMap), source.size()};
        backingFd = fd;
        sourceLocked = true;
        LOGW("JIT_DIAG: KCodeMemory initialized: source={} size=0x{:X} backed and locked",
             fmt::ptr(source.data()), source.size());
        return {};
    }

    Result KCodeMemory::MapImpl(u64 address, size_t size, memory::Permission permission, bool toOwner, u64 *allocated) {
        if (!size || size != source.size()) return result::InvalidSize;
        if (toOwner ? (permission != memory::Permission{true, false, false} && permission != memory::Permission{true, false, true})
                    : permission != memory::Permission{true, true, false})
            return result::InvalidNewMemoryPermission;
        auto process{owner.lock()};
        if (!process || !sourceLocked) return result::InvalidState;
        std::lock_guard lock{mutex};
        auto &mapped{toOwner ? ownerAddress : writerAddress};
        if (mapped) return result::InvalidState;
        auto result{allocated ? process->memory.AllocateCodeMemoryAlias(backingFd, size, permission, toOwner, address)
                              : process->memory.MapCodeMemoryAlias(backingFd, {reinterpret_cast<u8 *>(address), size}, permission, toOwner)};
        if (result != Result{}) return result;
        mapped = address;
        if (allocated) *allocated = address;
        if (permission.x) SynchronizeLocked(0, size);
        return {};
    }

    Result KCodeMemory::Map(u64 address, size_t size, memory::Permission permission, bool toOwner) {
        return MapImpl(address, size, permission, toOwner, nullptr);
    }

    Result KCodeMemory::MapToOwner(size_t size, memory::Permission permission, u64 &address) {
        return MapImpl(0, size, permission, true, &address);
    }

    Result KCodeMemory::MapAnywhere(size_t size, memory::Permission permission, u64 &address) {
        return MapImpl(0, size, permission, false, &address);
    }

    Result KCodeMemory::Unmap(u64 address, size_t size, bool fromOwner) {
        if (size != source.size()) return result::InvalidSize;
        auto process{owner.lock()};
        if (!process) return result::InvalidState;
        std::lock_guard lock{mutex};
        auto &mapped{fromOwner ? ownerAddress : writerAddress};
        if (!mapped || mapped != address) return result::InvalidCurrentMemory;
        auto result{process->memory.UnmapCodeMemoryAlias({reinterpret_cast<u8 *>(address), size}, fromOwner)};
        if (result == Result{}) mapped = 0;
        return result;
    }

    void KCodeMemory::Synchronize(size_t offset, size_t size) {
        std::lock_guard lock{mutex};
        SynchronizeLocked(offset, size);
    }

    void KCodeMemory::SynchronizeLocked(size_t offset, size_t size) {
        if (!size || offset > source.size() || size > source.size() - offset) return;
        std::atomic_thread_fence(std::memory_order_release);
        auto *writer{reinterpret_cast<char *>(writableBacking.data() + offset)};
        __builtin___clear_cache(writer, writer + size);
        if (ownerAddress) {
            if (auto process{owner.lock()}) {
                auto *reader{process->memory.TranslateVirtualPointer<char *>(ownerAddress + offset)};
                __builtin___clear_cache(reader, reader + size);
            }
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    KCodeMemory::~KCodeMemory() {
        if (auto process{owner.lock()}) {
            if (writerAddress) process->memory.UnmapCodeMemoryAlias({reinterpret_cast<u8 *>(writerAddress), source.size()}, false);
            if (ownerAddress) process->memory.UnmapCodeMemoryAlias({reinterpret_cast<u8 *>(ownerAddress), source.size()}, true);
            if (sourceLocked) process->memory.UnlockRegionForCodeMemory(source);
        }
        if (writableBacking.valid()) munmap(writableBacking.data(), writableBacking.size());
        if (backingFd >= 0) close(backingFd);
    }
}
