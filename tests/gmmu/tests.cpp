// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
namespace skyline {
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
template<class T, size_t Extent=std::dynamic_extent> struct span : std::span<T,Extent> {
    using std::span<T,Extent>::span;
    span(std::span<T,Extent> v):std::span<T,Extent>(v){}
    bool valid() const { return this->data()!=nullptr; }
    void copy_from(span other) { std::copy(other.begin(),other.end(),this->begin()); }
};
template<class T> span(T*,size_t)->span<T>;
template<class... T> struct exception : std::runtime_error {
    exception(const char* message,T&&...):std::runtime_error(message){}
};
template<class... T> exception(const char*,T&&...)->exception<T...>;
using SharedSpinLock=std::shared_mutex; // Android lock transport only.
namespace util {
template<class T> constexpr T DivideCeil(T x,T y) { return (x+y-1)/y; }
template<class T,class U> constexpr T AlignUp(T x,U y) { return (x+y-1)&~(y-1); }
template<class T> concept TrivialObject=std::is_trivially_copyable_v<T> && !requires(T v){v.data();};
}
}
#define TRACE_EVENT(...)
#include "segment_table.h"
#include "address_space.h"
#include "address_space.inc"
using namespace skyline;
using MM=FlatMemoryManager<u64,0,20,12,16>;
constexpr u64 Page=4096;
void CheckSparse() {
    MM mm; std::vector<u8> memory(Page,0x53);
    mm.Map(Page,memory.data(),Page);
    mm.Map(Page*2,MM::SparsePlaceholderAddress(),Page,{true});
    auto mappings=mm.TranslateRange(Page,Page*2);
    assert(mappings.size()==2 && mappings[0].data()==memory.data());
    assert(mappings[1].data()==nullptr && mappings[1].size()==Page); // Never feed a sentinel to CreateMirrors.
}
void CheckRemap() {
    MM mm; std::vector<u8> a(Page*8,0x11), b(Page,0x22);
    mm.Map(Page,a.data(),a.size());
    mm.Map(Page*3,b.data(),Page);
    auto [head,offset]=mm.LookupBlock(Page);
    assert(offset==0 && head.data()==a.data() && head.size()==Page*2);
    auto range=mm.TranslateRange(Page*2,Page*3);
    assert(range.size()==3 && range[0].size()==Page && range[1].data()==b.data() && range[2].data()==a.data()+Page*3);
    mm.Unmap(Page*3,Page);
    assert(!mm.LookupBlock(Page*3).first.valid());
    assert(mm.LookupBlock(Page*4).first.data()==a.data()+Page*3);
}
void CheckL2Tail() {
    SegmentTable<u64,1<<20,12,16> table;
    table.Set(0,1<<16,7);
    table.Set(4096*3,4096*4,9);
    assert(table[4096*3]==9 && table[4096*2]==7 && table[4096*4]==7);
    table.Set(1<<16,2<<16,11);
    table.Set(0,4096*17,3);
    assert(table[4096*18]==11); // Demotion must preserve the untouched L2 tail.
}
void CheckRandomMappings() {
    MM mm;
    std::vector<u8> physical(Page*512);
    struct Expected { u8 *pointer{}; MemoryMappingKind kind{MemoryMappingKind::Unmapped}; };
    std::array<Expected,256> model{};
    std::mt19937 rng(0xCAFE1234);
    for(unsigned iteration{};iteration<5000;++iteration) {
        const u64 first=rng()%240, count=1+rng()%15, mode=rng()%3, base=rng()%480;
        const auto before=mm.GetMappingGeneration();
        if(mode==0) mm.Unmap(first*Page,count*Page);
        else mm.Map(first*Page,mode==1?MM::SparsePlaceholderAddress():physical.data()+base*Page,count*Page,{mode==1});
        assert(mm.GetMappingGeneration()==before+1);
        for(u64 i=0;i<count;++i) model[first+i]={mode==2?physical.data()+(base+i)*Page:nullptr,
            mode==0?MemoryMappingKind::Unmapped:mode==1?MemoryMappingKind::Sparse:MemoryMappingKind::Resident};
        for(unsigned query{};query<5;++query) {
            u64 address=rng()%(Page*254), size=1+rng()%Page;
            auto mappings=mm.TranslateMappings(address,size);
            u64 consumed{};
            for(const auto &mapping:mappings) {
                assert(mapping.memory.size()>0);
                for(u64 i{};i<mapping.memory.size();++i) {
                    const auto va=address+consumed+i;
                    const auto &expected=model[va/Page];
                    assert(mapping.kind==expected.kind);
                    assert(mapping.memory.data()==nullptr ? expected.pointer==nullptr :
                        mapping.memory.data()+i==expected.pointer+va%Page);
                }
                consumed+=mapping.memory.size();
            }
            assert(consumed==size);
            const auto [block,offset]=mm.LookupBlock(address);
            const auto &expected=model[address/Page];
            assert(offset<block.size());
            assert(block.data()==nullptr ? expected.pointer==nullptr : block.data()+offset==expected.pointer+address%Page);
        }
    }
    std::cout<<"PASS: 5000 map/unmap/remap operations, 25000 fragmented queries\n";
}
void CheckEndpoints() {
    FlatMemoryManager<u32,0,32,12,17> mm;
    std::vector<u8> bytes(Page,0x42);
    mm.Map(0xfffff000U,bytes.data(),Page);
    assert(mm.TranslateRange(0xffffffffU,1).front().data()==bytes.data()+Page-1);
    mm.Unmap(0xfffff000U,Page);
    assert(!mm.LookupBlock(0xffffffffU).first.valid());
    MM small;
    bool invalid{};
    try { small.TranslateRange(1ULL<<40,1); } catch(const std::runtime_error&) { invalid=true; }
    assert(invalid && small.TranslateRange(0,0).empty());
    invalid=false;
    try { small.Map(1,bytes.data(),Page); } catch(const std::runtime_error&) { invalid=true; }
    assert(invalid && !small.LookupBlock(0).first.valid());
}
// Compile both production specializations, including 32-bit host1x endpoints.
namespace skyline {
template class FlatMemoryManager<u64,0,40,12,17>;
template class FlatMemoryManager<u32,0,32,12,17>;
}
int main(int argc,char**argv) {
    std::string name=argc>1?argv[1]:"all";
    if(name=="sparse" || name=="all") CheckSparse();
    if(name=="remap" || name=="all") CheckRemap();
    if(name=="l2" || name=="all") CheckL2Tail();
    if(name=="all") CheckEndpoints();
    if(name=="random" || name=="all") CheckRandomMappings();
    std::cout<<"PASS: "<<name<<'\n';
}
