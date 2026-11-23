#pragma once
// Built only into the diagnostic executable. The timed executable never replaces
// operator new, installs dispatch hooks, or updates these counters.
#include <cstdlib>
#include <new>
#include <cstddef>
namespace allocation_probe {
    inline thread_local bool enabled{};
    inline thread_local size_t calls{}, bytes{};
}
void *operator new(std::size_t n) {
    if (allocation_probe::enabled) { ++allocation_probe::calls; allocation_probe::bytes += n; }
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
#ifdef __cpp_sized_deallocation
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#endif
