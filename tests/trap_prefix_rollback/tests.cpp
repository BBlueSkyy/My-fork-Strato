// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <list>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sys/mman.h>
#include <thread>
#include <type_traits>
#include <vector>

thread_local bool rejectAllocations{};
thread_local int failAllocationAfter{-1};
void *operator new(std::size_t size) {
    if (rejectAllocations || failAllocationAfter == 0) throw std::bad_alloc{};
    if (failAllocationAfter > 0) --failAllocationAfter;
    if (auto pointer = std::malloc(size)) return pointer;
    throw std::bad_alloc{};
}
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
#define TRACE_EVENT(...) ((void)0)
namespace skyline {
    using u8 = uint8_t;
    template<typename T> using span = std::span<T>;
    namespace constant { constexpr size_t PageSize{4096}; }
    namespace util {
        template<typename T> T AlignDown(T value, size_t alignment) {
            if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(value) & ~(alignment - 1));
            else return value & ~(alignment - 1);
        }
        template<typename T> T AlignUp(T value, size_t alignment) {
            if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>((reinterpret_cast<uintptr_t>(value) + alignment - 1) & ~(alignment - 1));
            else return (value + alignment - 1) & ~(alignment - 1);
        }
    }
}
#include "interval.inc"
// Transparent counters around the real map traversal and real mprotect. They
// measure the production reprotection algorithm without replacing its behavior.
namespace skyline {
    size_t overlapPasses{};
    template<typename Address, typename Entry>
    class CountingIntervalMap : public IntervalMap<Address, Entry> {
        using Base = IntervalMap<Address, Entry>;
      public:
        template<typename Visitor>
        void ForEachRange(typename Base::Interval interval, Visitor &&visitor) {
            ++overlapPasses;
            Base::ForEachRange(interval, std::forward<Visitor>(visitor));
        }
    };
}
size_t protectionCalls{};
int CountMprotect(void *address, size_t size, int protection) {
    ++protectionCalls;
    return mprotect(address, size, protection);
}
#define IntervalMap CountingIntervalMap
#define mprotect CountMprotect
#include "trap.inc"
#undef mprotect
#undef IntervalMap

int main() {
    using namespace skyline;
    auto *memory = static_cast<u8 *>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    TrapManager traps;
    span<u8> prefix{memory, 4096}, expanded{memory, 8192};
    auto old = traps.CreateTrap({&prefix, 1}, [] {}, [] { return true; }, [] { return true; });
    traps.TrapRegions(old, true);
    // A trap removed during the out-of-trapMutex lock callback must let the
    // already observed protection fault retry its original memory instruction.
    std::optional<TrapHandle> temporary;
    temporary = traps.CreateTrap({&expanded, 1}, [&] { traps.DeleteTrap(*temporary); },
        [] { return false; }, [] { return false; });
    traps.TrapRegions(*temporary, false);
    const bool retry = traps.HandleTrap(memory + 4096, true);
    std::cout << "removed extension fault retried=" << retry << std::endl;
    // Teardown must work while unwinding an allocation failure, including pages
    // shared with the original prefix trap.
    auto cleanup = traps.CreateTrap({&expanded, 1}, [] {}, [] { return true; }, [] { return true; });
    traps.TrapRegions(cleanup, false);
    bool allocated{};
    rejectAllocations = true;
    try { traps.DeleteTrap(cleanup); } catch (const std::bad_alloc &) { allocated = true; }
    rejectAllocations = false;
    if (allocated) traps.DeleteTrap(cleanup);
    std::cout << "trap rollback allocated=" << allocated << std::endl;
    assert(traps.HandleTrap(memory, true));
    if (!allocated) {
        memory[0] = 11;
        memory[4096] = 22;
    }
    traps.DeleteTrap(old);
    bool leakedRegistration{};
    for (int failure{}; failure < 6; ++failure) {
        TrapManager failedTraps;
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> weak = token;
        LockCallback callback = [token] {};
        token.reset();
        std::optional<TrapHandle> result;
        bool rejected{};
        failAllocationAfter = failure;
        try { result = failedTraps.CreateTrap({&expanded, 1}, callback, [] { return true; }, [] { return true; }); }
        catch (const std::bad_alloc &) { rejected = true; }
        failAllocationAfter = -1;
        callback = {};
        if (result) failedTraps.DeleteTrap(*result);
        if (rejected && !weak.expired()) leakedRegistration = true;
    }
    std::cout << "failed trap registration leaked=" << leakedRegistration << std::endl;
    if (!retry || allocated || leakedRegistration) {
        munmap(memory, 8192);
        return 1;
    }
    bool slowCommonCase{};
    auto measured = [&](auto operation) {
        overlapPasses = protectionCalls = 0;
        rejectAllocations = true;
        operation();
        rejectAllocations = false;
        slowCommonCase |= overlapPasses != 1 || protectionCalls != 1;
    };
    auto single = traps.CreateTrap({&expanded, 1}, [] {}, [] { return true; }, [] { return true; });
    measured([&] { traps.TrapRegions(single, true); });
    measured([&] { traps.RemoveTrap(single); });
    auto exact = traps.CreateTrap({&expanded, 1}, [] {}, [] { return true; }, [] { return true; });
    measured([&] { traps.TrapRegions(single, true); });
    measured([&] { traps.TrapRegions(exact, true); });
    measured([&] { traps.RemoveTrap(exact); });
    measured([&] { traps.DeleteTrap(exact); });
    measured([&] { traps.DeleteTrap(single); });
    std::cout << "single/exact alias one-pass protection=" << !slowCommonCase << std::endl;
    munmap(memory, 8192);
    if (slowCommonCase) return 1;
    std::cout << "trap prefix rollback passed\n";
}
