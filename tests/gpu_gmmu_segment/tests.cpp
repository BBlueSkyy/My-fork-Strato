// SPDX-License-Identifier: MPL-2.0
#include <common/mapping_identity.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <utility>
#include <vector>
using u8=uint8_t;
namespace util {
template<class T,class U>constexpr T DivideCeil(T a,U b){return (a+b-1)/b;}
template<class T,class U>constexpr T AlignUp(T a,U b){return (a+b-1)&~(b-1);}
}
#include "segment_fixture.inc"
int main() {
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    constexpr size_t Page=4096,Big=0x20000,Base=0x405E20000,Selector=0x405E31400;
    std::vector<u8> bytes(4*Big);for(size_t i{};i<bytes.size();++i)bytes[i]=static_cast<u8>((i*7+i/257+17)%251);
    segment_test::Gmmu memory;memory.blocks={{0,nullptr,{}}};
    memory.Map(Base-Big,bytes.data(),2*Big);
    memory.Unmap(Base-Big,Big+Page);
    // feb00c3d incidentally repairs the lost L1 tail on this redundant map.
    // f3faa882 correctly avoids a false generation event but exposes the loss.
    const auto revision=memory.MappingRevision();
    memory.Map(Base+Page,bytes.data()+Big+Page,Big-Page);
    auto [block,offset]=memory.LookupBlock(Selector);
    assert(block.valid());
    assert(block.data()+offset==bytes.data()+Big+(Selector-Base));
    assert(std::equal(block.data()+offset,block.data()+offset+8192,bytes.data()+Big+(Selector-Base)));
    if(!std::getenv("GMMU_SEGMENT_REF"))assert(memory.MappingRevision()==revision);
    std::puts("PASS: captured selector address remains resident after partial unmap + unchanged remap; real table and independent bytes");
    if(std::getenv("GMMU_SEGMENT_REPLAY_ONLY"))return 0;
    // Correctness must hold before any redundant map, for every retained page.
    memory.Map(Base-Big,bytes.data(),2*Big);
    auto receipt=memory.mappingIdentity.Capture(Base+Page,Big-Page);
    memory.Unmap(Base-Big,Big+Page);
    for(size_t i=1;i<Big/Page;++i) {
        auto [part,at]=memory.LookupBlock(Base+i*Page+1024);
        assert(part.valid() && part.data()+at==bytes.data()+Big+i*Page+1024);
        assert(std::equal(part.data()+at,part.data()+at+256,bytes.data()+Big+i*Page+1024));
    }
    assert(receipt.IsCurrent());
    // Map replacement uses the same demotion tail; neither adjacent data nor
    // sparse metadata can disappear when only the prefix is overwritten.
    memory.Map(Base-Big,bytes.data(),2*Big);
    memory.Map(Base-Big,bytes.data()+2*Big,Big+Page);
    for(size_t i=1;i<Big/Page;++i) {
        auto [part,at]=memory.LookupBlock(Base+i*Page);
        assert(part.valid() && part.data()+at==bytes.data()+Big+i*Page);
    }
    memory.Map(Base-Big,reinterpret_cast<u8 *>(uintptr_t{0xCAFEBABE}),2*Big,{true});
    memory.Map(Base-Big,bytes.data(),Big+Page);
    assert(!memory.LookupBlock(Base+Page).first.valid());
    assert(memory.blockSegmentTable[Base+Page].extraInfo.sparseMapped);
    // Deterministic mutations compare all actual mapped-page bytes to an oracle
    // that knows only requested map/unmap operations, not either GMMU table.
    std::array<u8 *,128> oracle{};
    segment_test::SegmentTable<segment_test::Gmmu::Entry,(1ULL<<36),12,17> table;
    uint32_t random=0x172947U;
    auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned iteration{};iteration<512;++iteration) {
        const size_t first=next()%oracle.size(),count=1+next()%(oracle.size()-first);
        if(next()%4==0) {
            table.Set(Base-Big+first*Page,Base-Big+(first+count)*Page,{});
            for(size_t p=first;p<first+count;++p)oracle[p]=nullptr;
        } else {
            const size_t source=next()%(oracle.size()-count+1);
            table.Set(Base-Big+first*Page,Base-Big+(first+count)*Page,{Base-Big+first*Page,bytes.data()+source*Page,count*Page,{}});
            for(size_t p=first;p<first+count;++p)oracle[p]=bytes.data()+(source+p-first)*Page;
        }
        for(size_t p{};p<oracle.size();++p) {
            const auto address=Base-Big+p*Page+512;
            const auto &entry=table[address];
            assert(static_cast<bool>(entry.phys)==static_cast<bool>(oracle[p]));
            if(oracle[p]) {
                const auto actual=entry.phys+(address-entry.virt);
                assert(actual==oracle[p]+512);
                assert(std::equal(actual,actual+32,oracle[p]+512));
            }
        }
    }
    // BufferManager uses a pointer-valued 512-KiB L2 cell (128 L1 pages).
    // Preserve both pointer identity and its independently stored byte value.
    u8 markerA=0x31,markerB=0x72;
    segment_test::SegmentTable<u8 *,(1ULL<<22),12,19> pointers;
    constexpr size_t coarse=1ULL<<19;
    pointers.Set(0,2*coarse,&markerA);
    pointers.Set(0,coarse+Page,&markerB);
    for(size_t p=coarse+Page;p<2*coarse;p+=Page)assert(pointers[p]==&markerA && *pointers[p]==0x31);
    pointers.Set(0,coarse+2*Page,nullptr);
    for(size_t p=coarse+2*Page;p<2*coarse;p+=Page)assert(pointers[p]==&markerA && *pointers[p]==0x31);
    std::puts("PASS: retained dense/sparse tails, unchanged receipt and 512 page-oracle mutations with production mmap-backed segment table");
}
