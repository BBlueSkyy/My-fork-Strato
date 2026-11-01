#pragma once
// SPDX-License-Identifier: MPL-2.0
#include "host_types.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include "trap_manager.h"
static int lastProtection{};
static int Protect(void *p, size_t size, int protection) {
    lastProtection = protection;
    return ::mprotect(p, size, protection);
}
#define mprotect Protect
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
static void Check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Page {
    u8 *data = static_cast<u8 *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Page() { Check(data != MAP_FAILED, "mmap failed"); }
    ~Page() { munmap(data, 4096); }
};
