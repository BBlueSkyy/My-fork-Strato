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
    S handles;
    S::TextureSource source{2,48,0,2,48,0};
    handles.RecordTexture(K::TextureType,4,source,2);
    handles.RecordTexture(K::TextureInteger,4,source,0);
    handles.RecordTexture(K::SamplerCompare,4,source,3);
    int wordReads=0;
    auto readHandle=[&](unsigned stage,const S::TextureSource &input) {
        assert(stage==4);
        return input.ReadHandle([&](unsigned bank,unsigned offset) { assert(bank==2 && offset==48); ++wordReads; return 123u; });
    };
    assert(handles.Matches([](const S::Entry &) { assert(false); return 0u; },readHandle,
        [](K kind,unsigned handle) { assert(handle==123); return kind==K::TextureType?2u:kind==K::TextureInteger?0u:3u; }));
    assert(wordReads==1); // all metadata for this source shares one CBUF word
    assert(!handles.Matches([](const S::Entry &) { return 0u; },readHandle,
        [](K kind,unsigned) { return kind==K::TextureType?7u:0u; }));
    S::TextureSource split{2,32,20,3,64,0};
    int splitReads=0;
    assert(split.ReadHandle([&](unsigned bank,unsigned offset) { ++splitReads; assert((bank==2 && offset==32)||(bank==3 && offset==64)); return bank==2?0x123u:0x456u; })==0x12300456u);
    assert(splitReads==2);
    std::puts("PASS normalized specialization, dedup, handles-first, selective metadata checks");
}
