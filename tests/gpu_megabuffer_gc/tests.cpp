// SPDX-License-Identifier: MPL-2.0
#include "support.h"
#include "megabuffer.h"
#include <future>
#include <cstdlib>
#include <new>

// Test-only global allocation fault injection: fail exactly one allocation.
static thread_local int failAllocationAfter{-1};
[[gnu::noinline]] void *operator new(std::size_t size) {
    if (failAllocationAfter == 0) { failAllocationAfter = -1; throw std::bad_alloc{}; }
    if (failAllocationAfter > 0) --failAllocationAfter;
    if (auto pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void *pointer) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
[[gnu::noinline]] void *operator new[](std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
[[gnu::noinline]] void operator delete[](void *pointer, std::size_t) noexcept { ::operator delete(pointer); }
using namespace skyline::gpu;
using HostBuffer = memory::Buffer;
static int failures;
static void check(bool value, const char *message) {
    std::cout << (value ? "PASS " : "FAIL ") << message << '\n';
    if (!value) ++failures;
}
struct Fixture {
    GPU gpu;
    std::vector<std::shared_ptr<vk::raii::Device>> devices;
    std::vector<std::shared_ptr<FenceCycle>> cycles;
    std::unique_ptr<MegaBufferAllocator> allocator{std::make_unique<MegaBufferAllocator>(gpu)};
    std::shared_ptr<FenceCycle> begin() {
        auto device = std::make_shared<vk::raii::Device>();
        auto cycle = std::make_shared<FenceCycle>(*device, vk::Fence{}, vk::Semaphore{});
        devices.push_back(device); cycles.push_back(cycle);
        return cycle;
    }
    void complete(bool worker = false) {
        auto cycle = cycles.back();
        cycle->NotifySubmitted(); devices.back()->signal();
        if (worker) { std::thread thread([&] { cycle->Wait(true); }); thread.join(); }
        else cycle->Wait(true);
    }
    void step(bool worker = false) { auto cycle = begin(); allocator->Allocate(cycle, MegaBufferChunkSize - PAGE_SIZE); complete(worker); }
    ~Fixture() { for (auto &cycle : cycles) cycle->Cancel(); }
};
static void burstReclamation() {
    Fixture fixture;
#ifdef GC_IMPLEMENTED
    const auto initial{fixture.allocator->GetDiagnostics()};
    check(initial.allocatedChunks == 1 && initial.poolChunks == 1 && initial.rollovers == 0,
          "GC diagnostics start with one allocated chunk and no rollovers");
#endif
    auto cycle = fixture.begin();
    for (int i = 0; i < 8; ++i) fixture.allocator->Allocate(cycle, MegaBufferChunkSize - PAGE_SIZE);
    check(HostBuffer::live.size() == 8, "temporary pending burst owns eight 25 MiB chunks");
    fixture.complete();
    for (int i = 0; i < 240; ++i) fixture.step();
    check(HostBuffer::live.size() == 8, "recently used surplus retains a long reuse grace");
    auto recordThread = std::this_thread::get_id();
    for (int i = 0; i < 1000; ++i) fixture.step(true);
    std::cout << "METRIC burst retained chunks=" << HostBuffer::live.size() << " bytes=" << HostBuffer::live.size() * MegaBufferChunkSize << '\n';
    check(HostBuffer::live.size() == 4, "aged burst converges to four-chunk reserve");
#ifdef GC_IMPLEMENTED
    const auto stats{fixture.allocator->GetDiagnostics()};
    check(stats.allocatedChunks == 8 && stats.peakChunks == 8 && stats.poolChunks == 4,
          "GC diagnostics track allocated, peak and retained pool chunks");
    check(stats.retirementsQueued == 4 && stats.candidatesInspected > 0 && stats.rollovers >= 256,
          "GC diagnostics distinguish queued retirements from resource use");
#endif
    check(HostBuffer::destroyed > 0 && HostBuffer::lastDestruction != recordThread, "retired chunk destruction runs on fence completion worker");
    auto allocated = HostBuffer::allocated;
    for (int i = 0; i < 1000; ++i) fixture.step();
    check(HostBuffer::allocated == allocated, "steady working set reuses reserve without allocation churn");
}
static void pendingAndMultipleSubmissions() {
    Fixture fixture;
    auto first = fixture.begin();
    auto firstAllocation = fixture.allocator->Allocate(first, 1024);
    auto second = fixture.begin();
    auto secondAllocation = fixture.allocator->Allocate(second, MegaBufferChunkSize - PAGE_SIZE - 1024);
    check(firstAllocation.buffer == secondAllocation.buffer, "one chunk is used by multiple chained submissions");
    for (int i = 0; i < 7; ++i) fixture.allocator->Allocate(second, MegaBufferChunkSize - PAGE_SIZE);
    // Never submit these two cycles. Allocation must not block on either one.
    for (int i = 0; i < 1400; ++i) fixture.step();
    check(HostBuffer::live.contains(firstAllocation.buffer), "unsubmitted predecessor and chained consumer protect old chunk");
    check(HostBuffer::live.size() >= 8, "pending chunks are never counted as disposable idle storage");
    first->Cancel(); second->Cancel();
    for (int i = 0; i < 1400; ++i) fixture.step();
    check(HostBuffer::live.size() == 4, "cancelled original users eventually permit surplus reclamation");
}
static void outstandingRetirementAndCancellation() {
    Fixture fixture;
    auto first = fixture.begin();
    for (int i = 0; i < 8; ++i) fixture.allocator->Allocate(first, MegaBufferChunkSize - PAGE_SIZE);
    fixture.complete();
    for (int i = 0; i < 270; ++i) fixture.step();
    // If ageing already removed some nodes, create and complete another burst.
    auto burst = fixture.begin();
    for (int i = 0; i < 8; ++i) fixture.allocator->Allocate(burst, MegaBufferChunkSize - PAGE_SIZE);
    fixture.complete();
    // Stop on the first completed buffer destruction, then age less than a full grace.
    for (int i = 0; i < 250; ++i) fixture.step();
    auto before = HostBuffer::live.size();
    auto recording = fixture.begin();
    // Retire at most one node while repeatedly filling other completed chunks.
    for (int i = 0; i < 6; ++i) fixture.allocator->Allocate(recording, MegaBufferChunkSize - PAGE_SIZE);
    check(HostBuffer::live.size() >= before, "collection never destroys storage on recording thread");
    auto destroyedBefore = HostBuffer::destroyed;
    recording->Cancel();
    check(HostBuffer::destroyed - destroyedBefore <= 1, "cancellation releases at most one outstanding retired chunk");
    for (int i = 0; i < 1400; ++i) fixture.step();
    check(HostBuffer::live.size() == 4, "cancellation leaves collection cursor and reuse pool valid");
}
#ifdef GC_IMPLEMENTED
static void deferredOwnershipAndFailures() {
    Fixture fixture;
    auto first = fixture.begin();
    for (int i = 0; i < 8; ++i) fixture.allocator->Allocate(first, MegaBufferChunkSize - PAGE_SIZE);
    fixture.complete();
    auto current = fixture.begin();
    auto &pool = *fixture.allocator;
    pool.rollover = 1000;
    for (int failAfter : {0, 1}) {
        pool.collectionCursor = pool.chunks.begin();
        failAllocationAfter = failAfter;
        pool.CollectIdleChunk(current);
        failAllocationAfter = -1;
        check(pool.chunks.size() == 8 && pool.retiredChunk.expired() && HostBuffer::live.size() == 8,
              failAfter == 0 ? "retirement allocation failure preserves pool ownership" : "dependency allocation failure preserves pool ownership");
    }
    check(pool.GetDiagnostics().retirementAllocationFailures == 2,
          "GC diagnostics count optional retirement allocation failures");
    pool.collectionCursor = pool.chunks.begin();
    pool.CollectIdleChunk(current);
    check(pool.chunks.size() == 7 && HostBuffer::live.size() == 8 && !pool.retiredChunk.expired(),
          "removed chunk stays alive as a fence dependency");
    for (int i = 0; i < 100; ++i) pool.CollectIdleChunk(current);
    check(pool.chunks.size() == 7 && pool.retiredChunk.lock()->size() == 1,
          "stalled retirement bounds deferred garbage to exactly one chunk");
    current->Cancel();
    check(pool.retiredChunk.expired() && HostBuffer::live.size() == 7,
          "failed submission cancellation releases safe retired storage");
    pool.CollectIdleChunk(current);
    check(pool.chunks.size() == 7, "already cancelled current cycle cannot accept retirement");
    current = fixture.begin();
    pool.collectionCursor = pool.activeChunk;
    pool.CollectIdleChunk(current);
    check(pool.chunks.size() == 7, "active chunk is excluded even after a long idle epoch");
    pool.collectionCursor = pool.chunks.begin();
    pool.chunks.front().lastUseRollover = pool.rollover;
    pool.CollectIdleChunk(current);
    check(pool.chunks.size() == 7, "recently reused inactive chunk receives a new grace period");
}
#endif
static void submittedFenceAndChainedCompletion() {
    Fixture fixture;
    auto predecessor = fixture.begin();
    auto allocation = fixture.allocator->Allocate(predecessor, 1024);
    predecessor->NotifySubmitted();
    auto consumer = fixture.begin();
    fixture.allocator->Allocate(consumer, MegaBufferChunkSize - PAGE_SIZE - 1024);
    for (int i = 0; i < 7; ++i) fixture.allocator->Allocate(consumer, MegaBufferChunkSize - PAGE_SIZE);
    consumer->NotifySubmitted();
    fixture.devices.back()->signal(); // Its own GPU fence is ready, predecessor isn't.
    check(!consumer->Poll(false), "ready consumer fence does not bypass incomplete chained predecessor");
    for (int i = 0; i < 1400; ++i) fixture.step();
    check(HostBuffer::live.contains(allocation.buffer), "submitted but unsignalled original use cannot be collected");
    fixture.devices.front()->signal();
    consumer->Wait(true);
    for (int i = 0; i < 1400; ++i) fixture.step();
    check(HostBuffer::live.size() == 4, "real fence completion and predecessor completion allow deferred collection");
}
static void basicAllocation() {
    Fixture fixture;
    auto cycle = fixture.begin();
    std::vector<u8> data{1, 2, 3, 4};
    auto first = fixture.allocator->Push(cycle, skyline::span<u8>{data.data(), data.size()});
    auto aligned = fixture.allocator->Allocate(cycle, 12, true);
    check(first.offset == PAGE_SIZE && std::equal(data.begin(), data.end(), first.region.begin()), "Push preserves bytes and reserved first page");
    check(aligned.offset % PAGE_SIZE == 0 && aligned.offset >= first.offset + first.region.size(), "page-aligned allocations do not overlap");
    for (int i = 0; i < 1500; ++i) { fixture.complete(); fixture.step(); }
    check(HostBuffer::live.size() == 1, "small pools never grow to the collection reserve floor");
#ifdef GC_IMPLEMENTED
    const auto stats{fixture.allocator->GetDiagnostics()};
    check(stats.poolChunks == 1 && stats.retirementsQueued == 0,
          "GC diagnostics distinguish low-usage games with no collection");
#endif
}
static void benchmark() {
    Fixture fixture;
    auto cycle = fixture.begin();
    volatile vk::DeviceSize result{};
    constexpr int count = 200000;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) result = fixture.allocator->Allocate(cycle, 64).offset;
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    std::cout << "BENCH fast allocation ns/op=" << double(elapsed) / count << " result=" << result << '\n';
    fixture.complete();
    auto burst = fixture.begin();
    for (int i = 0; i < 8; ++i) fixture.allocator->Allocate(burst, MegaBufferChunkSize - PAGE_SIZE);
    fixture.complete();
    std::vector<long long> durations;
    durations.reserve(3000);
    for (int i = 0; i < 3000; ++i) {
        auto next = fixture.begin();
        auto begin = std::chrono::steady_clock::now();
        fixture.allocator->Allocate(next, MegaBufferChunkSize - PAGE_SIZE);
        durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        fixture.complete();
    }
    std::sort(durations.begin(), durations.end());
    std::cout << "BENCH rollover ns p50=" << durations[1500] << " p95=" << durations[2850]
              << " p99=" << durations[2970] << " max=" << durations.back() << '\n';
}
int main(int argc, char **) {
    if (argc > 1) { benchmark(); return 0; }
    burstReclamation();
    check(HostBuffer::live.empty(), "shutdown releases retained pool after cancellation");
    pendingAndMultipleSubmissions(); outstandingRetirementAndCancellation(); basicAllocation();
    submittedFenceAndChainedCompletion();
#ifdef GC_IMPLEMENTED
    deferredOwnershipAndFailures();
#endif
    check(HostBuffer::live.empty() && HostBuffer::allocated == HostBuffer::destroyed, "shutdown destroys every allocation exactly once");
    return failures ? 1 : 0;
}
