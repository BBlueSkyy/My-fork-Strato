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
void CheckAdjacentUnmaps() {
    constexpr size_t Base=0x405E20000,Size=0x10000,Selector=0x405E31400;
    std::array<std::vector<u8>,9> bytes;
    segment_test::Gmmu memory;memory.blocks={{0,nullptr,{}}};
    for(size_t i{};i<bytes.size();++i) {
        bytes[i].resize(Size,static_cast<u8>(0x31+i));
        memory.Map(Base-2*Size+i*Size,bytes[i].data(),Size);
    }
    auto vectorAt=[&](size_t address) {
        auto next=std::upper_bound(memory.blocks.begin(),memory.blocks.end(),address,[](size_t value,const auto &block){return value<block.virt;});
        const auto &block=*std::prev(next);
        return block.phys && !block.extraInfo.sparseMapped ? block.phys+(address-block.virt) : nullptr;
    };
    assert(vectorAt(Selector)==bytes[3].data()+0x1400);
    std::vector<std::pair<size_t,size_t>> callbacks;
    memory.unmapCallback=[&](size_t address,size_t size){callbacks.emplace_back(address,size);};
    auto old=memory.mappingIdentity.Capture(Base+Size,Size);
    memory.Unmap(Base,Size);
    memory.Unmap(Base+Size,Size);
    assert(callbacks.size()==2);
    assert(callbacks[0]==std::make_pair(Base,Size) && callbacks[1]==std::make_pair(Base+Size,Size));
    assert(!vectorAt(Selector)); // Must agree with the now-unmapped fast table.
    assert(!memory.LookupBlock(Selector).first.valid() && !old.IsCurrent());
    const auto revision=memory.MappingRevision();
    memory.Map(Base,bytes[2].data(),Size);
    const auto afterFirst=memory.MappingRevision();
    memory.Map(Base+Size,bytes[3].data(),Size);
    assert(memory.MappingRevision()>afterFirst && afterFirst>revision); // The second map cannot be a false no-op.
    assert(callbacks.size()==4 && callbacks[2]==std::make_pair(Base,Size) && callbacks[3]==std::make_pair(Base+Size,Size));
    auto fresh=memory.mappingIdentity.Capture(Base+Size,Size);
    assert(fresh.IsCurrent() && !old.IsCurrent() && fresh.GenerationAt(Selector)!=old.GenerationAt(Selector));
    auto [part,offset]=memory.LookupBlock(Selector);
    assert(part.valid() && part.data()+offset==bytes[3].data()+0x1400);
    assert(std::equal(part.data()+offset,part.data()+offset+8192,bytes[3].data()+0x1400));
    const auto stable=memory.MappingRevision();memory.Map(Base+Size,bytes[3].data(),Size);
    assert(memory.MappingRevision()==stable && callbacks.size()==4 && fresh.IsCurrent());
    std::puts("PASS: captured adjacent Unmap/Map sequence keeps main/fast tables consistent and restores selector bytes");
}
void CheckExtendHoleLeft() {
    constexpr size_t Base=0x405E00000,Page=4096;
    for(bool sparse:{false,true}) {
        std::vector<u8> bytes(8*Page,0x31);
        segment_test::Gmmu memory;memory.blocks={{0,nullptr,{}}};
        memory.Map(Base,sparse ? reinterpret_cast<u8 *>(uintptr_t{0xCAFEBABE}) : bytes.data(),bytes.size(),{sparse});
        std::vector<std::pair<size_t,size_t>> callbacks;
        memory.unmapCallback=[&](size_t address,size_t size){callbacks.emplace_back(address,size);};
        memory.Unmap(Base+3*Page,2*Page);
        auto head=memory.mappingIdentity.Capture(Base,2*Page);
        auto tail=memory.mappingIdentity.Capture(Base+5*Page,3*Page);
        memory.Unmap(Base+2*Page,3*Page); // Move one existing hole boundary left; erase range is empty.
        assert(head.IsCurrent() && tail.IsCurrent());
        assert(callbacks.size()==2 && callbacks[0]==std::make_pair(Base+3*Page,2*Page) && callbacks[1]==std::make_pair(Base+2*Page,3*Page));
        for(size_t i{};i<8;++i) {
            auto [part,offset]=memory.LookupBlock(Base+i*Page);
            assert(part.valid()==(!sparse && (i<2 || i>=5)));
            if(part.valid())assert(part.data()+offset==bytes.data()+i*Page);
        }
        assert(memory.blocks.size()==5); // sentinel, prefix, hole, retained tail, final hole.
    }
    std::puts("PASS: overlapping Unmap extends dense/sparse hole without rejecting valid empty erase or losing retained intervals");
}
void CheckGmmuMutationOracle() {
    constexpr size_t Page=4096,Count=128,Base=0x405E00000;
    struct Expected {u8 *physical{};bool sparse{};};
    std::array<Expected,Count> oracle{};
    std::vector<u8> bytes(Count*Page);
    for(size_t i{};i<bytes.size();++i)bytes[i]=static_cast<u8>((i*19+i/257+31)%251);
    segment_test::Gmmu memory;memory.blocks={{0,nullptr,{}}};
    uint32_t random=0x172947U;
    auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned iteration{};iteration<1024;++iteration) {
        const size_t first=next()%Count,count=1+next()%(Count-first),mode=next()%5;
        if(mode==0) {
            const auto before=memory.blocks;
            try {memory.Unmap(Base+first*Page,count*Page);} catch(...) {
                std::fprintf(stderr,"UNMAP ORACLE rejected iteration=%u first=%zu count=%zu\n",iteration,first,count);
                for(const auto &b:before)std::fprintf(stderr,"block=%zx physical=%d sparse=%d\n",b.virt,static_cast<bool>(b.phys),b.extraInfo.sparseMapped);
                throw;
            }
            for(size_t p=first;p<first+count;++p)oracle[p]={};
        } else if(mode==1) {
            memory.Map(Base+first*Page,reinterpret_cast<u8 *>(uintptr_t{0xCAFEBABE}),count*Page,{true});
            for(size_t p=first;p<first+count;++p)oracle[p]={nullptr,true};
        } else {
            const size_t source=next()%(Count-count+1);
            memory.Map(Base+first*Page,bytes.data()+source*Page,count*Page);
            for(size_t p=first;p<first+count;++p)oracle[p]={bytes.data()+(source+p-first)*Page,false};
        }
        for(size_t b=1;b<memory.blocks.size();++b) {
            assert(memory.blocks[b-1].virt<memory.blocks[b].virt);
            assert(memory.blocks[b-1].phys || memory.blocks[b].phys);
        }
        for(size_t p{};p<Count;++p) {
            const size_t address=Base+p*Page+512;
            auto nextBlock=std::upper_bound(memory.blocks.begin(),memory.blocks.end(),address,[](size_t a,const auto &b){return a<b.virt;});
            const auto &block=*std::prev(nextBlock);
            const bool resident=block.phys && !block.extraInfo.sparseMapped;
            const bool sparse=block.phys && block.extraInfo.sparseMapped;
            auto [part,offset]=memory.LookupBlock(address);
            assert(resident==static_cast<bool>(oracle[p].physical));
            assert(sparse==oracle[p].sparse && part.valid()==resident);
            if(resident) {
                const auto actual=block.phys+(address-block.virt);
                assert(actual==oracle[p].physical+512 && part.data()+offset==actual);
                assert(std::equal(actual,actual+32,oracle[p].physical+512));
            }
        }
    }
    std::puts("PASS: 1024 overlapping dense/sparse/unmap operations agree in main/fast tables and independent page/byte oracle");
}
int main() {
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    if(std::getenv("GMMU_EXTEND_HOLE_PROBE")){CheckExtendHoleLeft();return 0;}
    if(std::getenv("GMMU_ADJACENT_UNMAP_PROBE")){CheckAdjacentUnmaps();return 0;}
    if(!std::getenv("GMMU_SEGMENT_REPLAY_ONLY")) {CheckAdjacentUnmaps();CheckExtendHoleLeft();CheckGmmuMutationOracle();}
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
