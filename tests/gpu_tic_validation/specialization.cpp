// SPDX-License-Identifier: MPL-2.0
#include <gpu/interconnect/common/pipeline_specialization.h>
#include <cassert>
#include <cstdio>
using skyline::gpu::interconnect::PipelineSpecialization;
int main() {
    using S = PipelineSpecialization; using K = S::Kind;
    S a,b;
    a.Record(K::TextureType,0,7,0,2);
    a.Record(K::ConstantBuffer,4,2,48,0);
    a.Record(K::ConstantBuffer,4,2,48,0);
    b.Record(K::ConstantBuffer,4,2,48,0);
    b.Record(K::TextureType,0,7,0,2);
    assert(a == b);
    int reads=0;
    assert(!a.Matches([&](const S::Entry &entry) { ++reads; assert(entry.kind==K::ConstantBuffer); return 1u; }));
    assert(reads==1); // stale handle must stop before reading an old TIC
    assert(a.Matches([](const S::Entry &entry) { return entry.value; }));
    assert(!a.Matches([](const S::Entry &entry) { return entry.kind==K::TextureType ? 7u : entry.value; }));
    for (auto kind : {K::TextureType,K::TextureInteger,K::SamplerCompare}) {
        S s; s.Record(kind,0,0,0,2);
        assert(!s.Matches([](const S::Entry &) { return 3u; }));
    }
    S empty; assert(empty.Matches([](const S::Entry &) { assert(false); return 0u; }));
    std::puts("PASS normalized specialization, dedup, handles-first, selective metadata checks");
}
