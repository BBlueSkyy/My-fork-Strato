// SPDX-License-Identifier: MPL-2.0
// Compile with the unmodified production TrapManager and IntervalMap.
#include "trap_manager.h"
#include <sys/mman.h>
#include <csignal>
#include <unistd.h>
#include <future>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <optional>
#include <atomic>
#include <array>
#include <chrono>
using namespace skyline;
using namespace std::chrono_literals;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
struct Page {
    u8 *data{static_cast<u8 *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))};
    Page() { require(data != MAP_FAILED, "mmap"); }
    ~Page() { munmap(data, 4096); }
};
struct Resource {
    TrapManager &manager;
    std::optional<TrapHandle> handle;
    std::atomic<unsigned> &destroyed;
    Resource(TrapManager &manager, std::atomic<unsigned> &destroyed) : manager{manager}, destroyed{destroyed} {}
    ~Resource() { if (handle) manager.DeleteTrap(*handle); ++destroyed; }
};
void lastOwner(bool write, bool retry, bool throwing) {
    Page page;
    TrapManager manager;
    std::atomic<unsigned> destroyed{};
    auto owner{std::make_shared<Resource>(manager, destroyed)};
    std::weak_ptr<Resource> weak{owner};
    std::promise<void> entered, release;
    auto released{release.get_future().share()};
    unsigned calls{};
    auto callback = [&] {
        auto resource{weak.lock()};
        if (!resource) return true;
        if (++calls == 1) {
            entered.set_value();
            released.wait();
            if (throwing) throw std::runtime_error("callback exception");
            if (retry) return false;
        }
        return true;
    };
    std::array<span<u8>, 1> regions{{{page.data, 4096}}};
    owner->handle = manager.CreateTrap(regions, [] {}, callback, callback
#ifdef TRAP_HAS_OWNER
        , weak
#endif
    );
    manager.TrapRegions(*owner->handle, false);
    auto result{std::async(std::launch::async, [&] {
        try { return manager.HandleTrap(page.data, write); }
        catch (const std::runtime_error &) { return throwing; }
    })};
    entered.get_future().wait();
    owner.reset(); // Callback now owns the last reference on the old implementation.
    release.set_value();
    require(result.wait_for(2s) == std::future_status::ready, "deadlock after final callback owner");
    const bool handled{result.get()};
    require(handled, "the trap fault must be resolved before releasing the resource");
    if (retry) require(calls == 2, "retry lost its resource before the callback completed");
    require(weak.expired() && destroyed == 1, "final owner was not released exactly once");
    require(!manager.HandleTrap(page.data, true), "deleted callback remains in interval map");
}
void protections() {
    Page page;
    TrapManager manager;
    unsigned reads{}, writes{};
    std::array<span<u8>, 1> regions{{{page.data, 4096}}};
    auto a{manager.CreateTrap(regions, [] {}, [&] { ++reads; return true; }, [&] { ++writes; return true; })};
    auto b{manager.CreateTrap(regions, [] {}, [&] { ++reads; return true; }, [&] { ++writes; return true; })};
    manager.TrapRegions(a, false);
    manager.TrapRegions(b, false);
    require(manager.HandleTrap(page.data, false) && reads == 2, "overlapping reads");
    manager.RemoveTrap(a);
    manager.TrapRegions(a, true); // Remove keeps the handle valid for later host sync.
    require(manager.HandleTrap(page.data, true) && writes == 2, "overlapping writes");
    manager.DeleteTrap(a);
    manager.TrapRegions(b, false);
    require(manager.HandleTrap(page.data, false) && reads == 3, "surviving trap");
    manager.DeleteTrap(b);
}

// Pause a real protection fault before dispatch, then delete its last trap.
// This is the retirement window introduced by runtime TextureManager eviction.
static std::atomic<bool> faultEntered{}, dispatchFault{};
static volatile sig_atomic_t faultCalls{};
static void delayedFault(int, siginfo_t *info, void *) {
    faultCalls = faultCalls + 1;
    faultEntered.store(true, std::memory_order_release);
    while (!dispatchFault.load(std::memory_order_acquire)) {}
    if (!TrapManager::TrapHandler(static_cast<u8 *>(info->si_addr), true
#ifdef TRAP_HAS_PROTECTION_RETRY
        , info->si_code == SEGV_ACCERR
#endif
    ))
        _exit(86); // Same fatal dispatch decision as NCE, without a core dump.
}
void retiredProtectionFault() {
    Page page;
    TrapManager manager;
    manager.InstallStaticInstance();
    std::array<span<u8>, 1> regions{{{page.data, 4096}}};
    auto handle{manager.CreateTrap(regions, [] {}, [] { return true; }, [] { return true; })};
    manager.TrapRegions(handle, true);
    struct sigaction action{}, previous{};
    action.sa_sigaction = delayedFault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGSEGV, &action, &previous) == 0, "sigaction");
    auto writer{std::async(std::launch::async, [&] { *static_cast<volatile u8 *>(page.data) = 0x73; })};
    const auto deadline{std::chrono::steady_clock::now() + 2s};
    while (!faultEntered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) _exit(87);
        std::this_thread::yield();
    }
    manager.DeleteTrap(handle); // No callback has had a chance to pin the texture.
    dispatchFault.store(true, std::memory_order_release);
    require(writer.wait_for(2s) == std::future_status::ready, "retired fault did not resume");
    writer.get();
    require(page.data[0] == 0x73 && faultCalls == 1, "retired fault did not retry its instruction");
    sigaction(SIGSEGV, &previous, nullptr);
#ifdef TRAP_HAS_PROTECTION_RETRY
    require(!TrapManager::TrapHandler(page.data, true), "non-protection fault was accepted");
    require(!TrapManager::TrapHandler(nullptr, true, true), "unmapped fault was accepted");
    require(mprotect(page.data, 4096, PROT_NONE) == 0, "protect none");
    require(!TrapManager::TrapHandler(page.data, true, true), "inaccessible fault was accepted");
    require(mprotect(page.data, 4096, PROT_READ | PROT_EXEC) == 0, "protect read");
    require(!TrapManager::TrapHandler(page.data, true, true), "read-only fault was accepted");
    require(mprotect(page.data, 4096, PROT_READ | PROT_WRITE) == 0, "protect no-exec");
    require(!TrapManager::TrapHandler(page.data, true, true), "non-executable fault was accepted");
#endif
    manager.UninstallStaticInstance();
    std::cout << "PASS real SIGSEGV delivered before final trap deletion: instruction resumed\n";
}
int main(int argc, char **) {
    if (argc > 1) { lastOwner(false, false, false); return 0; }
    retiredProtectionFault();
    protections();
    for (unsigned i{}; i < 30; ++i)
        for (bool write : {false, true}) {
            lastOwner(write, false, false);
            lastOwner(write, true, false);
            lastOwner(write, false, true);
        }
    std::cout << "PASS real TrapManager/IntervalMap: 180 final-owner interleavings, read/write/retry/exception; overlapping protections and reusable handles\n";
}
