// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/physical_mapping_tracker.h>
#include <soc/gm20b/gmmu.h>
#include <stdexcept>
#include <cassert>
#include <future>
#include <thread>
#include <iostream>
using skyline::gpu::texture::PhysicalMappingTracker;
namespace skyline::soc::gm20b {
struct TestExecutor {
    unsigned submits{}, preserves{};
    bool fail{};
    void Submit() { ++submits; if (fail) throw std::runtime_error("submit"); }
    void LockPreserve() { ++preserves; }
    void UnlockPreserve() { --preserves; }
};
struct TestManager {
    TestExecutor &executor;
    unsigned collects{};
    size_t CollectGarbage(PhysicalMappingTracker &) { assert(!executor.preserves); return ++collects; }
};
struct ChannelContext {
    std::shared_ptr<AddressSpaceContext> asCtx;
    TestExecutor executor;
    std::mutex globalChannelLock;
    TestManager textureManager{executor};
    std::unique_lock<std::recursive_mutex> textureMappingLock;
    u64 mappingGeneration{};
    void Lock();
    void Unlock();
};
#include "channel_collection.inc"
}
void CheckGmmuAndChannel() {
    using namespace skyline::soc::gm20b;
    auto tracker{std::make_shared<PhysicalMappingTracker>()};
    auto as{std::make_shared<AddressSpaceContext>(tracker)};
    std::vector<skyline::u8> bytes(8192);
    as->gmmu.Map(0x1000, bytes.data(), 4096);
    assert(as->gmmu.mapCalls == 1); // Real wrapper called its translation boundary.
    ChannelContext channel;
    channel.asCtx = as;
    channel.Lock();
    assert(channel.executor.submits == 0 && channel.executor.preserves == 1);
    auto mutate{std::async(std::launch::async, [&] { as->gmmu.Unmap(0x1000, 4096); })};
    assert(mutate.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    channel.Unlock(); mutate.get();
    channel.Lock();
    assert(channel.executor.submits == 1); // Generation flush before the next batch.
    channel.Unlock();
    channel.Lock();
    assert(channel.executor.submits == 1); // No generation change: no extra flush.
    channel.Unlock();
    as->gmmu.Map(0x2000, bytes.data(), 4096);
    as->gmmu.Map(0x2000, bytes.data()+4096, 4096); // Remap changes physical identity.
    channel.executor.fail = true;
    bool failed{};
    try { channel.Lock(); } catch (const std::runtime_error &) { failed = true; }
    assert(failed);
    auto lockCheck{std::async(std::launch::async, [&] {
        std::unique_lock outer{channel.globalChannelLock, std::try_to_lock};
        std::unique_lock inner{tracker->mutex, std::try_to_lock};
        return outer.owns_lock() && inner.owns_lock();
    })};
    assert(lockCheck.get()); // Exception path releases both mapping and channel locks.
    as->gmmu.failNext = true;
    try { as->gmmu.Map(0x4000, bytes.data(), 4096); } catch (const std::bad_alloc &) {}
    assert(!tracker->Valid()); // Partial/failed base mutation can never authorize collection.
}
int main() {
    CheckGmmuAndChannel();
    PhysicalMappingTracker mappings;
    auto a{mappings.Register()}, b{mappings.Register()};
    mappings.Map(a, 0x1000, 0x10000, 0x4000);
    mappings.Map(b, 0x8000, 0x11000, 0x1000);
    {
        std::scoped_lock lock{mappings.mutex};
        mappings.Prepare();
        assert(mappings.IsMapped(0x10000, 0x4000));
        const auto initial{mappings.Generation()};
        mappings.Map(a, 0x1000, 0, 0x4000);
        mappings.Prepare();
        assert(mappings.Generation() != initial);
        assert(mappings.IsMapped(0x11000, 0x1000)); // Another AS still owns this span.
        assert(!mappings.IsMapped(0x10000, 0x1000));
        mappings.Map(b, 0x8000, 0x30000, 0x1000); // Same VA, different physical allocation.
        mappings.Prepare();
        assert(!mappings.IsMapped(0x11000, 0x1000));
        assert(mappings.IsMapped(0x30000, 0x1000));
        mappings.Map(a, 0x1000, 0x10000, 0x4000);
        mappings.Map(a, 0x2000, 0, 0x1000);
        mappings.Prepare();
        assert(mappings.IsMapped(0x10000, 0x1000));
        assert(!mappings.IsMapped(0x11000, 0x1000));
        assert(mappings.IsMapped(0x12000, 0x2000));
        mappings.Map(a, 0x2000, 0x11000, 0x1000);
        mappings.Prepare();
        assert(mappings.IsMapped(0x11000, 0x1000)); // Split then exact remap.
    }
    std::promise<void> locked, release;
    auto gate{release.get_future().share()};
    auto batch{std::async(std::launch::async, [&] {
        std::scoped_lock lock{mappings.mutex};
        locked.set_value(); gate.wait();
        mappings.Prepare();
        assert(mappings.IsMapped(0x10000, 0x4000));
    })};
    locked.get_future().wait();
    auto unmap{std::async(std::launch::async, [&] { mappings.Map(a, 0x1000, 0, 0x4000); })};
    assert(unmap.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    release.set_value(); batch.get(); unmap.get();
    auto writer{std::async(std::launch::async, [&] {
        for (unsigned i{}; i < 1000; ++i) {
            mappings.Map(a, 0x1000, 0x10000, 0x4000);
            mappings.Map(a, 0x2000, 0, 0x1000);
        }
    })};
    for (unsigned i{}; i < 1000; ++i) {
        std::scoped_lock lock{mappings.mutex};
        mappings.Prepare();
        assert(mappings.IsMapped(0x30000, 0x1000)); // Other AS never changes.
    }
    writer.get();
    mappings.Remove(b);
    {
        std::scoped_lock lock{mappings.mutex};
        mappings.Prepare();
        assert(!mappings.IsMapped(0x30000, 0x1000));
        bool sweep{};
        const auto events{mappings.TakeChanges(sweep)};
        assert(sweep || !events.empty());
    }
    mappings.Map(a, UINT64_MAX - 2, 1, 32);
    assert(!mappings.Valid());
    std::cout << "PASS physical mappings: two ASes/VAs, partial unmap, VA reuse, batch exclusion, 1000 concurrent mutations, fail-closed overflow\n";
}
