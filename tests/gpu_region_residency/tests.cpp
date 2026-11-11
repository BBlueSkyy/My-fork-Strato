// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <shared_mutex>
#include <span>
#include <vector>
using u8 = uint8_t;
using VaType = size_t;
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> other) : std::span<T>(other) {}
    bool valid() const { return this->data() != nullptr; }
};
template<class T> span(T *,size_t) -> span<T>;
using TranslatedAddressRange = std::vector<span<u8>>;
#define TRACE_EVENT(...)
struct Gmmu {
    struct Extra { bool sparseMapped{}; };
    struct Block { size_t virt{}; u8 *phys{}; size_t extent{}; Extra extraInfo{}; };
    std::vector<Block> blocks;
    struct Table {
        Gmmu *owner;
        const Block &operator[](size_t address) const {
            const auto it=std::upper_bound(owner->blocks.begin(),owner->blocks.end(),address,
                [](size_t value,const auto &block) { return value<block.virt; });
            assert(it!=owner->blocks.begin());return *std::prev(it);
        }
    } blockSegmentTable{this};
    std::shared_mutex blockMutex;
    TranslatedAddressRange TranslateRangeImpl(VaType,VaType,std::function<void(span<u8>)>,bool *);
    #include "translation.inc"
};
#define MM_MEMBER(Type) Type Gmmu
#include "slow.inc"
int main() {
    std::vector<u8> bytes(8192);
    Gmmu memory;
    memory.blocks={{0,bytes.data(),4096,{}},{4096,bytes.data()+4096,4096,{}},{8192,nullptr,4096,{}}};
    bool resident=false;
    auto ranges=memory.TranslateRange(128,512,{},&resident);
    assert(resident && ranges.size()==1 && ranges[0].data()==bytes.data()+128);
    ranges=memory.TranslateRange(0,8192,{},&resident);
    assert(resident && ranges.size()==1 && ranges[0].size()==8192);
    // Sparse placeholders are non-null: pointer validity alone is insufficient.
    memory.blocks[1].phys=reinterpret_cast<u8 *>(uintptr_t{0xCAFEBABE});
    memory.blocks[1].extraInfo.sparseMapped=true;
    ranges=memory.TranslateRange(0,8192,{},&resident);
    assert(!resident && ranges.size()==2);
    ranges=memory.TranslateRange(4096,128,{},&resident);
    assert(!resident && !ranges.front().valid());
    // Residence can change; each translation proves its own snapshot.
    memory.blocks[1].phys=bytes.data()+4096;memory.blocks[1].extraInfo.sparseMapped=false;
    ranges=memory.TranslateRange(0,8192,{},&resident);
    assert(resident && ranges.size()==1);
    ranges=memory.TranslateRange(8192,128,{},&resident);
    assert(!resident && !ranges.front().valid());
    std::puts("PASS: production fast/fragmented GMMU residency proof, non-null sparse placeholder, remap and unmapped rejection");
}
