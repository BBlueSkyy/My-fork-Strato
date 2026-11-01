// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "host_types.h"
#ifndef VK_WHOLE_SIZE
#define VK_WHOLE_SIZE (~uint64_t{})
#endif
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "trap_manager.h"
#define TRACE_EVENT(...)
// Count real protection syscalls without instrumenting the Android product.
static std::atomic<size_t> regionProtectionCalls{};
static int CountRegionProtect(void* address, size_t size, int permission) {
    ++regionProtectionCalls;
    return ::mprotect(address, size, permission);
}
#define mprotect CountRegionProtect
#include "trap.inc"
#undef mprotect
namespace skyline {
    struct ArmContext {
        struct {
            alignas(16) unsigned char __reserved[4096]{};
        } uc_mcontext;
    };
#include "routing.inc"
} // namespace skyline
using namespace skyline;
static void Check(bool value, const char* text) {
    if (!value)
        throw std::runtime_error(text);
}
