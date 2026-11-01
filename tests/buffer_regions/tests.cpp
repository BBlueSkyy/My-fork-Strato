// SPDX-License-Identifier: MPL-2.0
#include "fixture.h"
#include <array>
#include <cstdlib>
#ifdef WITH_VULKAN
#undef VK_WHOLE_SIZE
#include "vulkan.inc"
#endif
using namespace std::chrono_literals;
#ifndef WITH_VULKAN
// Keep allocation interposition out of the Vulkan/LLVM shared-library process.
static thread_local unsigned failAllocation{};
[[gnu::noinline]] void* operator new(std::size_t size) {
    if (failAllocation && !--failAllocation)
        throw std::bad_alloc();
    if (auto p = std::malloc(size))
        return p;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* p) noexcept {
    std::free(p);
}
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
#endif

static void UnrelatedCpuPageDoesNotWait() {
    Fixture f;
    auto b = f.buffer;
    auto producer = std::make_shared<gpu::FenceCycle>();
    std::promise<void> release;
    auto gate = release.get_future().share();
    producer->complete = [&] {
        gate.wait();
        std::memset(b->backing->data(), 0x64, 4096);
    };
    f.GpuWrite(0, 2040, producer);
    auto store = std::async(std::launch::async, [&] { f.Store(4096 + 37, 0x9a); });
    const bool independent = store.wait_for(200ms) == std::future_status::ready;
    if (!independent) {
        release.set_value();
        store.get();
        Check(false, "CPU access to unchanged page waited for unrelated GPU writer");
    }
    store.get();
    Check(!producer->waits && b->backing->data()[4096 + 37] == 0x11, "CPU region write touched GPU backing or consumed fence");
#if HAS_BUFFER_REGIONS
    {
        std::scoped_lock lock{*b};
        b->SynchronizeHostForGpu();
    }
    Check(!producer->waits && b->HasStagedCpuWrites(), "unrelated submit moved the wait or discarded deferred CPU ownership");
#endif
    release.set_value();
    {
        std::scoped_lock lock{*b};
        Check(b->SynchronizeGuest(), "readback failed");
    }
    for (size_t i{}; i < Fixture::Size; ++i)
        Check(f.mirror[i] == (i < 4096 ? 0x64 : i == 4096 + 37 ? 0x9a : 0x11), "mixed CPU/GPU readback lost latest bytes");
    {
        std::scoped_lock lock{*b};
        b->SynchronizeHost();
    }
    for (size_t i{}; i < Fixture::Size; ++i)
        Check(b->backing->data()[i] == f.mirror[i], "mixed upload changed GPU bytes");
    std::puts("PASS disjoint CPU page proceeds with GPU writer pending; mixed download/upload preserves every byte");
}

#if HAS_BUFFER_REGIONS
static void StableGpuDirtyProtection() {
    Fixture f;
    auto b = f.buffer;
    auto producer = std::make_shared<gpu::FenceCycle>();
    producer->done = true;
    f.GpuWrite(0, 2040, producer);
    regionProtectionCalls = 0;
    for (unsigned i = 0; i < 2000; ++i)
        f.GpuWrite(0, 2040, producer);
    std::printf("Repeated GPU-dirty bindings: %zu protection syscalls\n", regionProtectionCalls.load());
    Check(regionProtectionCalls == 0, "stable GPU-dirty bindings repeated page protections");
#ifndef WITH_VULKAN
    gpu::BufferRegionState stableMap;
    stableMap.Reset(8192, 4096);
    failAllocation = 1;
    try {
        Check(!stableMap.EnsureBounds(0, 8192), "unchanged map added a boundary");
    } catch (...) {
        failAllocation = 0;
        throw;
    }
    failAllocation = 0;
#endif
    // A real authority change must still re-arm protection and recover GPU bytes.
    b->backing->data()[37] = 0x61;
    Check(f.Load(37) == 0x61, "stable marking weakened GPU page protection");
    f.GpuWrite(0, 2040, producer);
    b->backing->data()[37] = 0x72;
    Check(f.Load(37) == 0x72, "new GPU writer did not re-arm CPU-dirty page");
    std::puts("PASS stable GPU ownership avoids protection/reserve work; ownership changes still trap");
}
static void DemandReadAndZeroSize() {
    Fixture f;
    auto b = f.buffer;
    auto producer = std::make_shared<gpu::FenceCycle>();
    producer->complete = [&] { std::memset(b->backing->data(), 0x57, 4096); };
    f.GpuWrite(0, 2040, producer);
    {
        std::scoped_lock lock{*b};
        Check(b->SynchronizeGuestRegion(false, false, 200, 0), "empty download");
    }
    Check(!producer->waits && b->stagedRegions.Has(gpu::BufferRegionState::Owner::Gpu), "empty sync changed GPU ownership");
    u8 byte{};
    unsigned flushes{};
    {
        std::scoped_lock lock{*b};
        b->ReadImplStaged(false, [&] { ++flushes; }, {&byte, 1}, 5000);
    }
    Check(byte == 0x11 && !producer->waits && !flushes, "unrelated programmatic read waited or flushed");
    {
        std::scoped_lock lock{*b};
        b->ReadImplStaged(false, [&] { ++flushes; }, {&byte, 1}, 38);
    }
    Check(byte == 0x57 && producer->waits == 1 && flushes == 1, "required read did not wait/flush its producer");
    std::puts("PASS demand reads preserve necessary waits/flushes; zero-length readback changes no protection");
}
static void ZeroWriteRetainsProtection() {
    Fixture f;
    auto b = f.buffer;
    {
        std::scoped_lock lock{*b};
        b->PrepareStagedRange(0, 2040);
    }
    f.Store(5000, 0xa5);
    {
        std::scoped_lock lock{*b};
        Check(!b->WriteImplStaged({}, 37, {}), "zero-byte inline write requested GPU work");
        b->MarkStagedCpuWrite(37, 0);
        b->ReadImplStaged(false, {}, {}, 37);
    }
    f.Store(37, 0x4b);
    {
        std::scoped_lock lock{*b};
        Check(b->stagedRegions.Has(gpu::BufferRegionState::Owner::Cpu, 37, 1), "zero-byte write removed synced page protection");
        b->SynchronizeHost();
    }
    Check(b->backing->data()[37] == 0x4b && b->backing->data()[5000] == 0xa5, "guest store escaped regional tracking after zero-byte write");
    std::puts("PASS zero-byte inline/metadata writes retain page protection and later CPU upload");
}
static void RequiredFaultAndSharedTrap() {
    for (bool external : {false, true}) {
        Fixture f;
        auto b = f.buffer;
        std::optional<TrapHandle> other;
        unsigned callbacks{};
        if (external) {
            span<u8> mapping{f.guest, Fixture::Size};
            other = f.process.trap.CreateTrap(
                mapping, [] {}, [] { return true; },
                [&] {
                    ++callbacks;
                    return true;
                });
            f.process.trap.TrapRegions(*other, true);
        }
        auto producer = std::make_shared<gpu::FenceCycle>();
        std::promise<void> release;
        auto gate = release.get_future().share();
        producer->complete = [&] {
            gate.wait();
            std::memset(b->backing->data(), 0x72, 4096);
        };
        f.GpuWrite(0, 2040, producer);
        auto fault = std::async(std::launch::async, [&] {
            if (external) {
                f.Store(5000, 0x29);
                return u8{0x29};
            }
            return f.Load(37);
        });
        const bool waiting = fault.wait_for(50ms) != std::future_status::ready;
        release.set_value();
        auto result = fault.get();
        if (!external)
            Check(waiting && producer->waits == 1 && result == 0x72, "required dependency was bypassed");
        else
            Check(result == 0x29 && f.Load(37) == 0x72 && producer->waits == 1, "overlapping trap weakened producer protection");
        if (other) {
            Check(callbacks == 1, "overlapping external trap was skipped");
            f.process.trap.DeleteTrap(*other);
        }
    }
    std::puts("PASS required page faults and overlapping legacy traps preserve callbacks and producer protection");
}
static void PartialAndReplacedWriters() {
    Fixture f;
    auto b = f.buffer;
    f.Store(4096 + 3, 0xad);
    auto first = std::make_shared<gpu::FenceCycle>();
    first->complete = [&] { std::memset(b->backing->data(), 0x37, 4096); };
    f.GpuWrite(2, 2040, first);
    auto last = std::make_shared<gpu::FenceCycle>();
    last->complete = [&] {
        first->Wait();
        std::memset(b->backing->data() + 4096 + 100, 0x86, 4);
    };
    // Upload CPU-owned B (including bytes not overwritten by the partial writer)
    // before assigning it to a newer GPU writer; retain the existing full-cycle fence.
    f.GpuWrite(4096 + 100, 4, last);
    Check(first->waits == 1 && b->backing->data()[4096 + 3] == 0xad, "partial GPU write lost old CPU bytes");
    {
        std::scoped_lock lock{*b};
        b->SynchronizeGuest();
    }
    for (size_t i{}; i < Fixture::Size; ++i) {
        auto expected = i < 4096 ? 0x37 : i == 4096 + 3 ? 0xad : i >= 4096 + 100 && i < 4096 + 104 ? 0x86 : 0x11;
        Check(f.mirror[i] == expected, "replacement/partial writer byte mismatch");
    }
    Check(last->waits == 1, "new writer was not waited");
    std::puts("PASS partial GPU writes, page sharing and chained replacement preserve all bytes");
}
static void InlineAndDma() {
    Fixture f;
    auto b = f.buffer;
    auto first = std::make_shared<gpu::FenceCycle>();
    first->complete = [&] { std::memset(b->backing->data(), 0x49, 4096); };
    f.GpuWrite(0, 2040, first);
    u8 update[]{0x41, 0x42, 0x43};
    unsigned copies{};
    gpu::UsageTracker usage;
    {
        std::scoped_lock lock{*b};
        Check(b->WriteImplStaged(update, 5100, {}), "inline fallback was not requested");
        Check(!b->WriteImplStaged(update, 5100,
                                  [&] {
                                      ++copies;
                                      std::memcpy(b->backing->data() + 5100, update, 3);
                                  }),
              "inline fallback failed");
        Check(b->currentExecutionGpuDirty, "inline GPU write omitted read barrier state");
        b->SynchronizeGuest();
    }
    Check(copies == 1 && !std::memcmp(f.mirror + 5100, update, 3) && f.mirror[12] == 0x49, "inline write duplicated callback or lost sibling GPU bytes");
    // Distinct offsets sharing the same backing exercise actual staged DMA body.
    f.Store(6000, 0xbc);
    {
        std::scoped_lock lock{*b};
        b->CopyFromImplStaged(100, b.get(), 6000, 1, usage, [&] {
            ++copies;
            std::memcpy(b->backing->data() + 100, b->backing->data() + 6000, 1);
        });
        b->SynchronizeHost();
    }
    Check(f.mirror[100] == 0xbc && b->backing->data()[100] == 0xbc, "same-backing disjoint copy lost CPU source");
    std::puts("PASS inline GPU update executes once; staged same-backing copy preserves bytes");
}
static void WaitFailureAndInvalidation() {
    Fixture f;
    auto b = f.buffer;
    auto producer = std::make_shared<gpu::FenceCycle>();
    producer->complete = [] { throw std::runtime_error("injected completion failure"); };
    f.GpuWrite(0, 2040, producer);
    bool failed{};
    try {
        std::scoped_lock lock{*b};
        b->SynchronizeGuest();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    Check(failed && b->cycle == producer && b->stagedRegions.Has(gpu::BufferRegionState::Owner::Gpu), "failure discarded GPU ownership/cycle");
    producer->complete = [&] { std::memset(b->backing->data(), 0x94, 4096); };
    {
        std::scoped_lock lock{*b};
        b->SynchronizeGuest();
    }
    Check(f.mirror[37] == 0x94, "readback retry lost bytes");
    {
        std::scoped_lock lock{*b};
        b->Invalidate();
        Check(!b->guest && b->stagedTraps.empty(), "invalidate retained traps");
    }
    Check(!f.process.trap.HandleTrap(f.guest, true), "removed backing still has a callback");
    std::puts("PASS failed completion retains authority for retry; invalidation removes trap lifetime");
}
static void InvalidateDuringPendingFault() {
    Fixture f;
    auto b = f.buffer;
    auto producer = std::make_shared<gpu::FenceCycle>();
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    producer->complete = [&] {
        entered.set_value();
        gate.wait();
    };
    f.GpuWrite(0, 2040, producer);
    auto fault = std::async(std::launch::async, [&] { return f.process.trap.HandleTrap(f.guest + 37, true); });
    entered.get_future().get();
    {
        std::scoped_lock lock{*b};
        std::scoped_lock stateLock{b->stateMutex};
        b->Invalidate();
    }
    release.set_value();
    Check(!fault.get() && b->stagedTraps.empty(), "pending fault retained removed backing callbacks");
    f.Store(37, 0x4d);
    Check(f.mirror[37] == 0x4d, "invalidation left old page protection active");
    std::puts("PASS invalidation during an unlocked producer wait retires callbacks without deadlock");
}
static void ExistingHackPreservesCpuSiblings() {
    for (bool writes : {false, true}) {
        Fixture f;
        auto b = f.buffer;
        auto producer = std::make_shared<gpu::FenceCycle>();
        producer->done = true;
        f.GpuWrite(0, 2040, producer);
        f.Store(5000, 0xa7);
        *f.settings.enableFastGpuReadbackHack = true;
        *f.settings.enableFastReadbackWrites = writes;
        b->accumulatedGuestWaitTime = 1s;
        std::memset(b->backing->data(), 0x63, 4096);
        f.Store(30, 0x38);
        Check(f.mirror[5000] == 0xa7, "existing shortcut overwrote CPU-owned sibling");
        Check(!producer->waits, "existing optional shortcut did not follow prior policy");
    }
    std::puts("PASS both existing shortcut modes preserve unrelated CPU ownership (not used for safe-path measurements)");
}
static void ConcurrentIndependentAccesses() {
    for (unsigned repeat{}; repeat < 25; ++repeat) {
        Fixture f;
        auto b = f.buffer;
        auto producer = std::make_shared<gpu::FenceCycle>();
        producer->done = true;
        f.GpuWrite(0, 2040, producer);
        std::vector<std::future<void>> writers;
        for (unsigned j{}; j < 4; ++j)
            writers.push_back(std::async(std::launch::async, [&, j] { f.Store(4096 + j, 0x80 + j); }));
        for (auto& w : writers)
            w.get();
        Check(!producer->waits, "concurrent disjoint stores waited GPU");
        {
            std::scoped_lock lock{*b};
            b->SynchronizeGuest();
            b->SynchronizeHost();
        }
        for (unsigned j{}; j < 4; ++j)
            Check(b->backing->data()[4096 + j] == 0x80 + j, "concurrent CPU stores lost");
    }
    std::puts("PASS concurrent disjoint traps preserve stores over 25 repetitions");
}
static void CoalesceDeferredCpuData() {
    for (bool gpuWriter : {false, true}) {
        Fixture f;
        auto b = f.buffer;
        {
            std::scoped_lock lock{*b};
            b->PrepareStagedRange(0, 2040);
        }
        f.Store(5000, 0xca);
        auto producer = std::make_shared<gpu::FenceCycle>();
        producer->complete = [&] {
            if (gpuWriter)
                std::memset(b->backing->data(), 0x51, 4096);
        };
        if (gpuWriter)
            f.GpuWrite(0, 2040, producer);
        gpu::BufferManager manager{f.gpu, {}, 0, {}};
        gpu::BufferManager::LockedBuffers sources;
        sources.emplace_back(b, 7);
        b->BlockAllCpuBackingWrites(0, 2040);
        b->cycle = producer;
        auto merged = manager.CoalesceBuffers(span<u8>{f.guest, Fixture::Size}, sources, 7);
        merged->SynchronizeGuest();
        merged->SynchronizeHost();
        for (size_t i{}; i < Fixture::Size; ++i)
            Check(merged->backing->data()[i] == (i < 4096 && gpuWriter ? 0x51 : i == 5000 ? 0xca : 0x11), "coalescing backing lost deferred CPU sibling");
        Check(b->delegate->GetBuffer() == merged.buffer.get(), "coalescence failed to retarget views");
        Check(producer->waits == 1, "coalescence omitted old backing lifetime wait");
    }
    std::puts("PASS production coalescence preserves CPU-owned siblings for GPU writer and read-only DMA source");
}
#ifndef WITH_VULKAN
static void ReplacementAllocationFailure() {
    unsigned failures{};
    for (unsigned allocation = 1; allocation <= 24; ++allocation) {
        Fixture f;
        auto b = f.buffer;
        auto producer = std::make_shared<gpu::FenceCycle>();
        producer->done = true;
        f.GpuWrite(0, Fixture::Size, producer);
        std::memset(b->backing->data(), 0x6d, Fixture::Size);
        bool failed{};
        failAllocation = allocation;
        try {
            std::scoped_lock lock{*b};
            b->PrepareStagedRange(0, 2040);
        } catch (const std::bad_alloc&) {
            failed = true;
            ++failures;
        }
        failAllocation = 0;
        if (failed)
            Check(b->stagedTraps.size() == 1, "failed replacement removed original trap set");
        f.Store(5000, 0x89);
        {
            std::scoped_lock lock{*b};
            b->SynchronizeGuest();
            b->SynchronizeHost();
            b->Invalidate();
        }
        for (size_t i{}; i < Fixture::Size; ++i)
            Check(f.mirror[i] == (i == 5000 ? 0x89 : 0x6d), "failed replacement corrupted authority or bytes");
        Check(!f.process.trap.HandleTrap(f.guest, true) && !f.process.trap.HandleTrap(f.guest + 5000, true), "failed replacement left orphan callback");
    }
    Check(failures >= 6, "insufficient allocation-failure coverage");
    std::printf("PASS trap replacement retains old protection and recovers (%u injected allocation failures)\n", failures);
}
#endif
static void RegionMapDifferential() {
    gpu::BufferRegionState state;
    using Owner = gpu::BufferRegionState::Owner;
    state.Reset(12345, 4096);
    std::array<Owner, 4> expected{Owner::Cpu, Owner::Cpu, Owner::Cpu, Owner::Cpu};
    uint32_t seed = 0x1251;
    for (unsigned i{}; i < 2000; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        size_t begin = seed % 12345, end = begin + (seed / 12345) % (12346 - begin);
        auto owner = static_cast<Owner>(seed % 3);
        state.EnsureBounds(begin, end - begin);
        state.AssignExisting(begin, end - begin, owner);
        if (end != begin)
            for (size_t p = begin / 4096; p <= (end - 1) / 4096; ++p)
                expected[p] = owner;
        for (size_t p{}; p < 4; ++p)
            for (auto candidate : {Owner::Synced, Owner::Cpu, Owner::Gpu})
                Check(state.Has(candidate, p * 4096, std::min(size_t{4096}, size_t{12345} - p * 4096)) == (expected[p] == candidate),
                      "authority differential mismatch");
        Check(!state.Has(Owner::Gpu, begin, 0) && !state.Has(Owner::Cpu, begin, 0), "empty authority range intersects");
    }
    state.Use(4096, 1);
    state.Use(100, 1);
    state.Use(8192, 1);
    Check(state.Accesses().size() == 1 && state.Accesses()[0].begin == 0 && state.Accesses()[0].end == 12288, "access coalescing mismatch");
    auto old = state.Regions().size();
    bool rejected{};
    try {
        state.EnsureBounds(12344, 2);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    Check(rejected && state.Regions().size() == old, "invalid bounds mutated map");
    std::puts("PASS 2,000 randomized page-authority comparisons, partial last page, bounds and access coalescing");
}

#ifdef WITH_VULKAN
static void NativeIndependentRegion() {
    Vulkan vk;
    NativeBuffer backing(vk), output(vk);
    Fixture f;
    auto b = f.buffer;
    b->backing->external = backing.data;
    std::memcpy(backing.data, f.mirror, Fixture::Size);
    auto producer = std::make_shared<gpu::FenceCycle>();
    f.GpuWrite(0, 2040, producer);
    VkEvent event{};
    VkEventCreateInfo ec{};
    ec.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    VkCheck(vkCreateEvent(vk.device, &ec, nullptr, &event));
    auto cmd = vk.Begin();
    vkCmdWaitEvents(cmd, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(cmd, backing.buffer, 0, 2040, 0x6f6f6f6f);
    Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
    VkBufferCopy copy{0, 0, 4096};
    vkCmdCopyBuffer(cmd, backing.buffer, output.buffer, 1, &copy);
    Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    auto fence = vk.Submit(cmd);
    producer->complete = [&] { VkCheck(vkWaitForFences(vk.device, 1, &fence, true, UINT64_MAX)); };
    auto store = std::async(std::launch::async, [&] {
        f.Store(4096 + 37, 0xab);
        std::scoped_lock lock{*b};
        b->SynchronizeHostForGpu();
    });
    const bool independent = store.wait_for(200ms) == std::future_status::ready;
    if (!independent) {
        vkSetEvent(vk.device, event);
        store.get();
        vk.Retire(cmd, fence);
        vkDestroyEvent(vk.device, event, nullptr);
        Check(false, "native disjoint CPU access/upload waited unrelated writer");
    }
    store.get();
    const bool pending = vkGetFenceStatus(vk.device, fence) == VK_NOT_READY;
    const bool untouched = backing.data[4096 + 37] == 0x11 && !producer->waits;
    VkCheck(vkSetEvent(vk.device, event));
    {
        std::scoped_lock lock{*b};
        b->SynchronizeGuest();
        b->SynchronizeHost();
    }
    vk.Retire(cmd, fence);
    vkDestroyEvent(vk.device, event, nullptr);
    Check(pending && untouched && producer->waits == 1, "native demand dependency changed");
    for (size_t i{}; i < 4096; ++i)
        Check(output.data[i] == (i < 2040 ? 0x6f : 0x11), "GPU copy observed incorrect producer data");
    auto second = vk.Begin();
    Barrier(second, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy full{0, 0, Fixture::Size};
    vkCmdCopyBuffer(second, backing.buffer, output.buffer, 1, &full);
    Barrier(second, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    auto next = vk.Submit(second);
    vk.Retire(second, next);
    for (size_t i{}; i < Fixture::Size; ++i)
        Check(output.data[i] == (i < 2040 ? 0x6f : i == 4096 + 37 ? 0xab : 0x11), "GPU->CPU->GPU lost partial-region bytes");
    Check(!validationErrors, "Vulkan synchronization validation errors");
    b->backing->external = nullptr;
    std::puts("PASS native pending GPU writer overlaps CPU alias store and unrelated submit; required fence/readback/upload retained; byte-exact next GPU "
              "copy; zero sync-validation errors");
}
#endif

#endif
int main() {
    struct sigaction action {};
    action.sa_sigaction = Fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    Check(!sigaction(SIGSEGV, &action, nullptr), "sigaction");
    try {
        UnrelatedCpuPageDoesNotWait();
#if HAS_BUFFER_REGIONS
        StableGpuDirtyProtection();
        DemandReadAndZeroSize();
        ZeroWriteRetainsProtection();
        RequiredFaultAndSharedTrap();
        PartialAndReplacedWriters();
        InlineAndDma();
        WaitFailureAndInvalidation();
        InvalidateDuringPendingFault();
        ExistingHackPreservesCpuSiblings();
        ConcurrentIndependentAccesses();
        CoalesceDeferredCpuData();
        RegionMapDifferential();
#ifndef WITH_VULKAN
        ReplacementAllocationFailure();
#endif
#ifdef WITH_VULKAN
        NativeIndependentRegion();
#endif
#endif
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
