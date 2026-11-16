// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/physical_mapping_tracker.h>
#include <cassert>
#include <future>
#include <thread>
#include <iostream>
using skyline::gpu::texture::PhysicalMappingTracker;
int main() {
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
