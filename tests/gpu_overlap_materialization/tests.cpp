// SPDX-License-Identifier: MPL-2.0
// Only resource I/O is replaced; the materialization source boundary is production.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#include "../../app/src/main/cpp/skyline/gpu/texture/alias_group.h"
using ContextTag = uint64_t;
namespace skyline::gpu {
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class... T> std::runtime_error exception(const char *message, T...) { return std::runtime_error(message); }
namespace boost::container { template<class T, unsigned> using small_vector = std::vector<T>; }
struct Group {};
class Texture {
  public:
    enum class DirtyState { Clean, CpuDirty, GpuDirty } dirtyState{DirtyState::GpuDirty};
    std::atomic<ContextTag> tag{};
    std::atomic<ContextTag> pendingGpuWriteTag{};
    std::recursive_mutex resourceMutex, stateMutex;
    struct { std::shared_ptr<Group> load() const { return {}; } } aliasGroup;
    bool memoryFreed{}, guest{true}, pending{}, fail{};
    int guestValue{2}, gpuValue{3}, pendingValue{42};
    uint64_t gpuWriteSerial{1}, gpuReadbackSerial{};
    std::atomic<bool> aliasStale{};
    unsigned downloads{};
    std::function<void()> inDownload;
    bool LockWithTag(ContextTag value) {
        if (value && tag == value) return false;
        resourceMutex.lock(); tag = value; return true;
    }
    void lock() { resourceMutex.lock(); }
    void unlock() { pendingGpuWriteTag = 0; tag = 0; resourceMutex.unlock(); }
    void SynchronizeGuest(bool, bool) {
        if (dirtyState != DirtyState::GpuDirty) return;
        // Match master's early state publication; the source-boundary guard
        // must restore this state if recording/copying later fails.
        dirtyState = DirtyState::Clean; memoryFreed = false;
        if (inDownload) inDownload();
        if (fail) throw std::runtime_error("injected readback failure");
        guestValue = gpuValue; gpuReadbackSerial = gpuWriteSerial; ++downloads;
    }
    void Submit() { if (pending) gpuValue = pendingValue; pending = false; pendingGpuWriteTag = 0; tag = 0; }
    static void RegisterAliases(const std::vector<std::shared_ptr<Texture>> &, void *) {}
    void ReconcileAliases() {}
};

template<class T> struct ContextLock {
    T &resource; bool owned;
    ContextLock(ContextTag tag, T &resource) : resource{resource}, owned{resource.LockWithTag(tag)} {}
    ContextLock(ContextLock &&other) : resource{other.resource}, owned{std::exchange(other.owned, false)} {}
    ~ContextLock() { if (owned) resource.unlock(); }
    bool IsFirstUsage() const { return owned; }
};
class TextureManager {
  public:
    void FindOrCreate(const GuestTexture &) { throw std::logic_error("materialization fixture cannot replay a regional lookup"); }
    static void SynchronizeGuestOverlaps(std::vector<std::shared_ptr<Texture>>, ContextTag = 0);
};
#include "publication.inc"
struct CommandExecutor {
    std::vector<std::shared_ptr<Texture>> queued;
    unsigned submissions{};
    void Submit() { ++submissions; for (const auto &source : queued) source->Submit(); }
    void HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &);
};
#include "retry.inc"
using namespace std::chrono_literals;

void PendingProducer() {
    auto a{std::make_shared<Texture>()}; a->pending = true; a->tag = a->pendingGpuWriteTag = 7;
    CommandExecutor executor{{a}};
    unsigned retries{};
    try { TextureManager::SynchronizeGuestOverlaps({a}, 7); }
    catch (const TextureAliasReconciliationRequest &request) {
        ++retries; Check(!a->downloads && request.overlapReadbacks.size() == 1, "readback happened before the retry boundary");
        executor.HandleTextureBackingPromotion(request);
    }
    Check(retries == 1 && executor.submissions == 1 && a->guestValue == 42 && a->downloads == 1, "new overlapping texture consumed a pending writer's old image");
    TextureManager::SynchronizeGuestOverlaps({a}, 7);
    Check(a->downloads == 1, "clean source downloaded again");
}

void FailureAndLocks() {
    auto a{std::make_shared<Texture>()}; a->memoryFreed = true; a->fail = true;
    bool failed{};
    try { TextureManager::SynchronizeGuestOverlaps({a}); } catch (const std::runtime_error &) { failed = true; }
    Check(failed && a->dirtyState == Texture::DirtyState::GpuDirty && a->memoryFreed && !a->gpuReadbackSerial,
          "failed overlap publication lost retryable GPU authority");
    a->fail = false;
    std::promise<void> entered, release;
    auto gate{release.get_future()};
    a->inDownload = [&] { entered.set_value(); gate.get(); };
    auto publish{std::async(std::launch::async, [&] { TextureManager::SynchronizeGuestOverlaps({a}); })};
    entered.get_future().get();
    const bool stateFree{a->stateMutex.try_lock()}; if (stateFree) a->stateMutex.unlock();
    const bool resourceFree{a->resourceMutex.try_lock()}; if (resourceFree) a->resourceMutex.unlock();
    release.set_value(); publish.get();
    Check(!stateFree && !resourceFree, "publication did not hold resource/state locks against concurrent CPU traps");
    Check(a->guestValue == 3 && !a->memoryFreed && a->gpuReadbackSerial == 1, "readback retry did not complete");
}

void Boundaries() {
    TextureManager::SynchronizeGuestOverlaps({}, 7);
    auto clean{std::make_shared<Texture>()}; clean->dirtyState = Texture::DirtyState::Clean; clean->gpuReadbackSerial = 1; clean->tag = 7;
    TextureManager::SynchronizeGuestOverlaps({clean}, 7);
    Check(!clean->downloads, "coherent source added a readback");
    auto cpu{std::make_shared<Texture>()}; cpu->dirtyState = Texture::DirtyState::CpuDirty; cpu->gpuWriteSerial = cpu->gpuReadbackSerial = 0; cpu->tag = 7;
    TextureManager::SynchronizeGuestOverlaps({cpu}, 7);
    Check(!cpu->downloads, "fresh CPU data required a submission/readback");
    auto a{std::make_shared<Texture>()}, b{std::make_shared<Texture>()};
    a->tag = a->pendingGpuWriteTag = 7; a->pending = true; b->gpuWriteSerial = 2;
    bool retried{};
    try { TextureManager::SynchronizeGuestOverlaps({b, a}, 7); }
    catch (const TextureAliasReconciliationRequest &) { retried = true; }
    Check(retried && !a->downloads && !b->downloads, "one source published before all pending producers were checked");
    auto historical{std::make_shared<Texture>()};
    historical->tag = 7; historical->dirtyState = Texture::DirtyState::Clean;
    historical->gpuWriteSerial = 99; historical->gpuReadbackSerial = 3;
    TextureManager::SynchronizeGuestOverlaps({historical}, 7);
    historical->dirtyState = Texture::DirtyState::CpuDirty;
    TextureManager::SynchronizeGuestOverlaps({historical}, 7);
    Check(!historical->downloads, "historical alias serials caused a needless retry/readback");
}
}
int main() {
    try {
        skyline::gpu::PendingProducer(); skyline::gpu::FailureAndLocks(); skyline::gpu::Boundaries();
        std::puts("PASS: pending overlap producer, lock discipline, failed publication retry and no-op paths");
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
