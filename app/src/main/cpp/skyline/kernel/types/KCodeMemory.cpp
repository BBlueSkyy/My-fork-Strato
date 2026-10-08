// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

#include <android/sharedmem.h>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <kernel/results.h>
#include "KCodeMemory.h"
#include "KProcess.h"

namespace skyline::kernel::type {
    KCodeMemory::KCodeMemory(const DeviceState &state, span<u8> source)
        : KObject(state, KType::KCodeMemory), source{source} {}

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
        munmap(initMap, source.size());

        if (!state.process->memory.LockRegionForCodeMemory(source)) {
            LOGW("JIT_DIAG: CreateCodeMemory source validation/lock failed at {} (size=0x{:X})",
                 fmt::ptr(source.data()), source.size());
            close(fd);
            return result::InvalidCurrentMemory;
        }

        // Source is inaccessible while borrowed. Alias creation (ControlCodeMemory)
        // will reuse this exact fd, never a copy, in a later experiment.
        auto hostSource{state.process->memory.GetHostSpan(source)};
        void *mapped{mmap(hostSource.data(), hostSource.size(), PROT_NONE,
                          MAP_FIXED | MAP_SHARED, fd, 0)};
        if (mapped == MAP_FAILED) {
            LOGW("JIT_DIAG: failed to replace locked source mapping: {}", strerror(errno));
            state.process->memory.UnlockRegionForCodeMemory(source);
            close(fd);
            return result::OutOfMemory;
        }

        backingFd = fd;
        sourceLocked = true;
        LOGW("JIT_DIAG: KCodeMemory initialized: source={} size=0x{:X} backed and locked",
             fmt::ptr(source.data()), source.size());
        return {};
    }

    KCodeMemory::~KCodeMemory() {
        if (sourceLocked && state.process)
            state.process->memory.UnlockRegionForCodeMemory(source);

        if (backingFd >= 0)
            close(backingFd);
    }
}
