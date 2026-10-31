// SPDX-License-Identifier: MPL-2.0
#include "ssbo_diagnostics.h"
#include <cassert>
#include <memory>
#include <thread>
#include <vector>
using namespace skyline::gpu::ssbo_diagnostics;
int main() {
    // Same offset must not merge different shaders, pipelines, dispatches or physical buffers.
    auto t{std::make_unique<BindingTable>()};
    BindingKey key{{0x123, 0x456, {4,1,1}}, 0, 0, 0x280, 3, false};
    auto *first{t->Record(key, 8388608, 2097152, 2101248, 10, 20)};
    assert(first && first->calls == 1 && first->maxRequest == 8388608);
    for (unsigned axis{}; axis < 5; ++axis) {
        auto other{key};
        if (axis == 0) ++other.dispatch.shader;
        if (axis == 1) ++other.dispatch.pipeline;
        if (axis == 2) ++other.dispatch.groups[0];
        if (axis == 3) ++other.buffer;
        if (axis == 4) other.kind = 2;
        assert(t->Record(other, 4, 4, 4096, 0, 0) != first);
    }
    std::vector<std::thread> threads;
    for (unsigned n{}; n < 4; ++n) threads.emplace_back([&] {
        for (unsigned i{}; i < 1000; ++i) assert(t->Record(key, 4, 4, 4096, 1, 2) == first);
    });
    for (auto &thread : threads) thread.join();
    assert(first->calls == 4001 && first->requested == 8388608 + 16000);
    assert(first->viewed == 2097152 + 16000 && first->maxBacking == 2101248);
    assert(first->updateNs == 4010 && first->attachNs == 8020);
    for (uint64_t i{}; i < 4096; ++i) { auto k{key}; k.buffer = i + 100; t->Record(k, 1, 1, 1, 0, 0); }
    assert(t->dropped > 0);
    auto b{std::make_unique<BufferTable>()};
    b->Init(0, 8388608, 0x100000, false);
    // Operations preceding the first observed compute binding must survive.
    b->Copy(0, true, 8388608, 100);
    b->Wait(0, 200); b->TrapWait(0, 300); b->Dirty(0, 8388608); b->Mega(0, 128);
    std::array<bool,32768> related{}; b->ResolveComputeRelated(related); assert(!related[0]);
    b->Init(1, 8392704, 0x100000, false); b->Link(0, 1); b->Bind(1);
    b->ResolveComputeRelated(related); assert(related[0] && related[1]);
    b->Coalesce(1,1024,40); assert(b->Get(1)->coalesceBytes==1024 && b->Get(1)->coalesceNs==40);
    assert(b->Get(0)->uploadBytes == 8388608 && b->Get(0)->waitNs == 200 && b->Get(0)->trapWaitNs == 300);
    assert(b->Get(0)->successor == 2 && b->Get(0)->megaBytes == 128);
    // A graphics-only successor of a Compute buffer is still related, as is
    // another predecessor merged into that same physical backing.
    b->Init(2, 9000000, 0x100000, false); b->Link(1, 3);
    b->Init(3, 10000000, 0x100000, false); b->Link(2, 3); b->Coalesce(3,4096,99);
    b->ResolveComputeRelated(related); assert(related[0] && related[1] && related[2] && related[3]);
    assert(b->Get(3)->coalesceBytes==4096);
    b->Init(32768, 1, 0, false); assert(!b->Get(32768) && b->dropped == 1);
}
