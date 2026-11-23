// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace skyline {
    /**
     * @brief The last step of a multi-step host memory operation on this thread, used to locate native faults that leave no backtrace
     * @note Updating it costs a few stores and no syscalls. It is only ever *read* by `ReportFaultBreadcrumb` from the fault handler of the same thread.
     */
    struct FaultBreadcrumb {
        const char *step{}; //!< Static string naming the last step that was entered, nullptr when no operation is in flight
        uint64_t sequence{}; //!< Monotonic per-thread operation counter
        uintptr_t begin{}; //!< Page-aligned guest range being mirrored
        size_t size{};
        uintptr_t mirror{}; //!< Address of the mirror mapping, 0 until it exists
        size_t index{}, count{}; //!< Segment index / total segments of the operation
    };

    inline thread_local FaultBreadcrumb faultBreadcrumb{};

    /**
     * @brief Writes the thread's in-flight breadcrumb synchronously (straight to logcat), if there is one
     * @note Called from the unhandled-SIGSEGV path right before the fault is turned into an exception, so it avoids the asynchronous logger which can lose its queue on a crash
     */
    inline void ReportFaultBreadcrumb(const void *faultAddress) {
        const auto &crumb{faultBreadcrumb};
        if (!crumb.step)
            return;
        char line[256];
        const auto fault{reinterpret_cast<uintptr_t>(faultAddress)};
        std::snprintf(line, sizeof(line), "unhandled SIGSEGV addr=0x%zx last-step=%s seq=%llu seg=%zu/%zu range=0x%zx+0x%zx mirror=0x%zx in-mirror=%d in-range=%d",
                      static_cast<size_t>(fault), crumb.step, static_cast<unsigned long long>(crumb.sequence), crumb.index, crumb.count,
                      static_cast<size_t>(crumb.begin), crumb.size, static_cast<size_t>(crumb.mirror),
                      crumb.mirror && fault >= crumb.mirror && fault < crumb.mirror + crumb.size,
                      fault >= crumb.begin && fault < crumb.begin + crumb.size);
#ifdef __ANDROID__
        __android_log_write(ANDROID_LOG_ERROR, "SPARSE-MIRROR", line);
#else
        std::fprintf(stderr, "SPARSE-MIRROR %s\n", line);
#endif
    }
}
