// SPDX-License-Identifier: MPL-2.0
#include <common/mapping_identity.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <stdexcept>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <shared_mutex>
#include <span>
#include <vector>
using u8 = uint8_t;
using skyline::MappingIdentity;
using u64 = uint64_t;
using VaType = size_t;
using PaType = u8 *;
constexpr u8 *UnmappedPa=nullptr;
constexpr bool PaContigSplit=true;
template<class...Args> std::runtime_error exception(const char *message,Args&&...) { return std::runtime_error(message); }
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> other) : std::span<T>(other) {}
    bool valid() const { return this->data() != nullptr; }
    template<class U> span<U> cast() const { return {reinterpret_cast<U *>(this->data()),this->size_bytes()/sizeof(U)}; }
};
template<class T> span(T *,size_t) -> span<T>;
using TranslatedAddressRange = std::vector<span<u8>>;
#define TRACE_EVENT(...)
struct Gmmu {
    struct Extra { bool sparseMapped; };
    using ExtraBlockInfo=Extra;
    using MemoryManagerBlockInfo=Extra;
    std::function<void(size_t,size_t)> unmapCallback;
    struct Block { size_t virt{}; u8 *phys{}; Extra extraInfo{};
        Block()=default;
        Block(size_t virt,u8 *phys,Extra info):virt{virt},phys{phys},extraInfo{info}{}
        bool Unmapped() const { return !phys; }
        bool Mapped() const { return phys; }
        bool operator<(size_t value) const { return virt<value; }
    };
    size_t vaLimit{1ULL<<40};
    skyline::MappingIdentity mappingIdentity;
    uint64_t MappingRevision() const { return mappingIdentity.Revision(); }
    skyline::MappingIdentity::Epoch ReadMappingEpoch() const { return mappingIdentity.ReadEpoch(); }
    struct TrackedTranslation { TranslatedAddressRange mappings; skyline::MappingIdentity::Snapshot identity; bool fullyResident{}; };
    void MapLocked(size_t,u8 *,size_t,Extra);
    void UnmapLocked(size_t,size_t);
    std::vector<Block> blocks;
    struct Table {
        Gmmu *owner;
        struct Entry {size_t virt;u8 *phys;size_t extent;Extra extraInfo;};
        void Set(size_t,size_t,Entry) {}
        Entry operator[](size_t address) const {
            const auto it=std::upper_bound(owner->blocks.begin(),owner->blocks.end(),address,
                [](size_t value,const auto &block) { return value<block.virt; });
            assert(it!=owner->blocks.begin());
            const auto &block{*std::prev(it)};return {block.virt,block.phys,(it==owner->blocks.end()?owner->vaLimit:it->virt)-block.virt,block.extraInfo};
        }
    } blockSegmentTable{this};
    std::shared_mutex blockMutex;
    TranslatedAddressRange TranslateRangeImpl(VaType,VaType,std::function<void(span<u8>)>,bool *);
    #include "translation.inc"
    #include "identity.inc"
};
#define MM_MEMBER(Type) Type Gmmu
#include "slow.inc"
#define MAP_MEMBER(Type) Type Gmmu
#include "map.inc"
int main() {
    std::vector<u8> a(3*4096,0x31),b(3*4096,0x72);
    Gmmu memory;memory.blocks={{0,nullptr,{}}};
    memory.Map(0x1000,a.data(),a.size());
    auto original=memory.TranslateRangeTracked(0x1000,a.size());
    assert(original.fullyResident && original.identity.IsCurrent());
    const auto epoch=memory.mappingIdentity.Revision();
    memory.Map(0x2000,a.data()+4096,4096); // Exact subrange no-op.
    assert(memory.mappingIdentity.Revision()==epoch && original.identity.IsCurrent());
    memory.Map(0x2000,b.data()+4096,4096);
    assert(!original.identity.IsCurrent() && original.identity.IsCurrent(0x1000,4096));
    assert(original.identity.IsCurrent(0x3000,4096));
    auto changed=memory.TranslateRangeTracked(0x1000,a.size());
    assert(changed.fullyResident && changed.mappings.size()==3);
    assert(changed.mappings[1].data()==b.data()+4096);
    assert(changed.identity.GenerationAt(0x1000)==original.identity.GenerationAt(0x1000));
    assert(changed.identity.GenerationAt(0x2000)!=original.identity.GenerationAt(0x2000));
    memory.Map(0x1000,a.data(),a.size()); // Full range over mixed mappings changes only the middle.
    assert(!original.identity.IsCurrent() && !changed.identity.IsCurrent());
    auto reused=memory.TranslateRangeTracked(0x1000,a.size());
    assert(reused.identity.GenerationAt(0x1000)==original.identity.GenerationAt(0x1000));
    assert(reused.identity.IsCurrent() && reused.mappings.size()==1);
    memory.Unmap(0x2000,4096);
    assert(!reused.identity.IsCurrent());
    auto absent=memory.TranslateRangeTracked(0x1000,a.size());
    assert(!absent.fullyResident && !absent.mappings[1].valid());
    const auto absentEpoch=memory.mappingIdentity.Revision();memory.Unmap(0x2000,4096);
    assert(memory.mappingIdentity.Revision()==absentEpoch && absent.identity.IsCurrent());
    memory.Map(0x2000,reinterpret_cast<u8 *>(uintptr_t{0xCAFEBABE}),4096,{true});
    auto sparse=memory.TranslateRangeTracked(0x1000,a.size());
    assert(!sparse.fullyResident && !sparse.mappings[1].valid());
    memory.Unmap(0x2000,4096);
    const auto emptySparseEpoch=memory.mappingIdentity.Revision();memory.Unmap(0x2000,4096);
    assert(memory.mappingIdentity.Revision()==emptySparseEpoch);
    // A valid prefix followed by the final absent block must not dereference
    // blocks.end() when descriptor capacity extends beyond the last mapping.
    memory.Map(0x9000,a.data(),4);
    memory.blocks.shrink_to_fit();
    auto tail=memory.TranslateRangeTracked(0x9000,16);
    assert(!tail.fullyResident && tail.mappings.size()==2);
    assert(tail.mappings[0].size()==4 && tail.mappings[0].data()==a.data());
    assert(!tail.mappings[1].valid() && tail.mappings[1].size()==12);
    std::puts("PASS: production GMMU map/unmap/no-op, partial translation generations, P->Q->P and sparse gaps");
}
