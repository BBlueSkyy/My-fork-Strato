// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <mutex>
#include <string>

// Shared, capture-only state. Do not mutate the process environment using
// setenv() while Vulkan threads call getenv(): POSIX does not guarantee safety.
// An atomic fast path keeps normal non-capture sessions effectively untouched.
namespace skyline::gpu::renderdoc_diag {
    inline std::atomic<bool> &Enabled() {
        static std::atomic<bool> enabled{false};
        return enabled;
    }
    // Shared write lock across the TIC and synchronization recorders.
    inline std::mutex &WriteMutex() {
        static std::mutex mutex;
        return mutex;
    }
    inline std::mutex &PathMutex() {
        static std::mutex mutex;
        return mutex;
    }
    inline std::string &PathValue() {
        static std::string value;
        return value;
    }
    inline void SetPath(std::string value) {
        std::scoped_lock lock(PathMutex());
        PathValue() = std::move(value);
        Enabled().store(!PathValue().empty(), std::memory_order_release);
    }
    inline std::string CurrentPath() {
        if (!Enabled().load(std::memory_order_acquire))
            return {};
        std::scoped_lock lock(PathMutex());
        return PathValue();
    }
}
