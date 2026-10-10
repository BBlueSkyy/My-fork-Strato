// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <fcntl.h>
#include <limits.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace skyline::hle {
    inline bool ReadProbeMemory(void *destination, const void *source, std::size_t size) {
        iovec local{destination, size};
        iovec remote{const_cast<void *>(source), size};
        if (syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL) == static_cast<ssize_t>(size))
            return true;
        // Some app sandboxes deny process_vm_readv even for the same process.
        // A private nonblocking pipe also performs fault-safe kernel copies.
        // Bound the transfer to PIPE_BUF so neither operation needs to wait.
        if (!size || size > PIPE_BUF)
            return false;
        int descriptors[2];
        if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK))
            return false;
        const auto written{write(descriptors[1], source, size)};
        const bool copied{written == static_cast<ssize_t>(size) &&
            read(descriptors[0], destination, size) == static_cast<ssize_t>(size)};
        close(descriptors[0]);
        close(descriptors[1]);
        return copied;
    }
}
