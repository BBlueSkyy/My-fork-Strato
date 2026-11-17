// SPDX-License-Identifier: MPL-2.0
#include <common/mapping_identity.h>
#include <cassert>
#include <future>
#include <cstdio>
#include <chrono>
#include <thread>
using namespace std::chrono_literals;
int main() {
    skyline::MappingIdentity identity;
    auto whole=identity.Capture(0x1000,0x3000);
    auto left=identity.Capture(0x1000,0x1000);
    const auto generation=left.GenerationAt(0x1000);
    assert(generation && whole.GenerationAt(0x1000)==generation);
    assert(whole.IsCurrent() && left.IsCurrent());
    { auto mutation=identity.Invalidate(0x2000,0x1000); }
    assert(!whole.IsCurrent() && left.IsCurrent());
    assert(whole.IsCurrent(0x1000,0x1000) && whole.IsCurrent(0x3000,0x1000));
    assert(!whole.IsCurrent(0x2000,0x1000));
    auto replacement=identity.Capture(0x2000,0x1000);
    assert(replacement.IsCurrent() && replacement.GenerationAt(0x2000)!=generation);
    { auto mutation=identity.Invalidate(0x2000,0x1000); }
    auto reused=identity.Capture(0x2000,0x1000);
    assert(!replacement.IsCurrent() && reused.GenerationAt(0x2000)!=replacement.GenerationAt(0x2000));
    auto leftAgain=identity.Capture(0x1000,0x1000);
    assert(leftAgain.GenerationAt(0x1000)==generation);
    // A mutation cannot pass a short validated publication lease.
    std::future<void> changing;
    std::promise<void> started;
    {
        auto guard=left.Acquire();assert(guard && guard.Matches(leftAgain));
        changing=std::async(std::launch::async,[&] {
            started.set_value();auto mutation=identity.Invalidate(0x1000,0x1000);
        });
        started.get_future().get();
        assert(changing.wait_for(50ms)==std::future_status::timeout);
    }
    assert(changing.wait_for(2s)==std::future_status::ready);changing.get();
    assert(!left.IsCurrent() && reused.IsCurrent());
    // Closing a translation owner does not leave a live numerical-address identity.
    skyline::MappingIdentity::Snapshot retained;
    { skyline::MappingIdentity temporary;retained=temporary.Capture(0x1000,0x1000);assert(retained.IsCurrent()); }
    assert(!retained.IsCurrent());
    #ifdef IDENTITY_RACE_PROBE
    // The test runner inserts a scheduling boundary after the first validity
    // load in an otherwise unchanged header. No hook enters production builds.
    skyline::MappingIdentity raced;auto snapshot=raced.Capture(0x1000,4096);
    std::promise<void> atLoad,resume;auto resumed=resume.get_future();std::thread::id reader;
    skyline::identityValidationHook=[&] {
        if(std::this_thread::get_id()==reader) {atLoad.set_value();resumed.wait();}
    };
    auto validating=std::async(std::launch::async,[&] {reader=std::this_thread::get_id();return snapshot.IsCurrent();});
    atLoad.get_future().get();
    {auto changed=raced.Invalidate(0x1000,4096);}
    assert(!snapshot.IsCurrent());resume.set_value();
    assert(!validating.get());skyline::identityValidationHook={};
    #endif
    // Hard limits fail before producing a receipt; expired generation metadata
    // is reclaimed at the ceiling instead of retaining unbounded history.
    skyline::MappingIdentity bounded;
    std::vector<skyline::MappingIdentity::Snapshot> active;
    active.reserve(skyline::MappingIdentity::MaxCells);
    for(size_t i{};i<skyline::MappingIdentity::MaxCells;++i) active.push_back(bounded.Capture(i*4,i ? 1 : 3));
    bool limited{};
    try { bounded.Capture(skyline::MappingIdentity::MaxCells*4,1); } catch(const std::length_error &) {limited=true;}
    assert(limited && active.front().IsCurrent());
    auto first=active.front();limited=false;
    try {auto split=bounded.Invalidate(1,1);} catch(const std::length_error &) {limited=true;}
    assert(limited && first.IsCurrent()); // Split failure must not retire the old identity.
    active.clear();assert(bounded.Capture(skyline::MappingIdentity::MaxCells*4,1).IsCurrent());
    {auto split=bounded.Invalidate(1,1);}
    assert(!first.IsCurrent() && first.IsCurrent(0,1) && first.IsCurrent(2,1));
    // Exceed the per-snapshot bound, then retry a representable prefix. A
    // rejected Capture must not invalidate the still-live source receipts.
    skyline::MappingIdentity fragmented;
    std::vector<skyline::MappingIdentity::Snapshot> fragments;
    for(size_t i{};i<=skyline::MappingIdentity::MaxReceiptRanges;++i)
        fragments.push_back(fragmented.Capture(i*2,2));
    limited=false;
    try {fragmented.Capture(0,fragments.size()*2);} catch(const std::length_error &) {limited=true;}
    assert(limited && fragments.front().IsCurrent() && fragments.back().IsCurrent());
    assert(fragmented.Capture(0,skyline::MappingIdentity::MaxReceiptRanges*2).IsCurrent());
    std::puts("PASS: interval generations, partial invalidation, address ABA, short publication lease and owner lifetime");
}
