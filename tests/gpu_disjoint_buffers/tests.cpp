// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <functional>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <tuple>
#include <vector>
using u8=uint8_t; using u64=uint64_t;
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> value):std::span<T>(value){}
    span(std::nullptr_t,size_t count):std::span<T>(static_cast<T*>(nullptr),count){}
    bool valid() const { return this->data()!=nullptr; }
};
namespace constant { constexpr u64 PageSize=4096; }
namespace util {
template<class T> T AlignDown(T value,u64 alignment) { return value & ~(alignment-1); }
template<class T> T AlignUp(T value,u64 alignment) { return (value+alignment-1)&~(alignment-1); }
}
struct exception : std::runtime_error { using std::runtime_error::runtime_error; };
using GuestBuffer=span<u8>;
enum class MemoryMappingKind { Resident, Sparse, Unmapped };
struct MemoryMapping { span<u8> memory; MemoryMappingKind kind; };
struct Gmmu {
    struct Mapping { u64 address; span<u8> bytes; MemoryMappingKind kind; };
    std::vector<Mapping> mappings; u64 generation{};
    void Set(u64 address,span<u8> bytes,MemoryMappingKind kind=MemoryMappingKind::Resident) {
        std::erase_if(mappings,[&](auto &m){return m.address==address;});
        mappings.push_back({address,bytes,kind}); ++generation;
    }
    u64 GetMappingGeneration() { return generation; }
    std::pair<span<u8>,u64> LookupBlock(u64 address) {
        for(auto &m:mappings) if(address>=m.address && address-m.address<m.bytes.size()) return {m.bytes,address-m.address};
        return {{},0};
    }
    std::vector<MemoryMapping> TranslateMappings(u64 address,u64 size) {
        if(size>~u64{}-address) throw std::runtime_error("range overflow");
        std::vector<MemoryMapping> result;
        while(size) {
            bool found=false;
            for(auto &m:mappings) if(address>=m.address && address-m.address<m.bytes.size()) {
                auto offset=address-m.address, count=std::min<u64>(size,m.bytes.size()-offset);
                result.push_back({{m.bytes.data()?m.bytes.data()+offset:nullptr,count},m.kind});
                address+=count; size-=count; found=true; break;
            }
            if(!found) { result.push_back({{nullptr,size},MemoryMappingKind::Unmapped}); break; }
        }
        return result;
    }
};
struct Buffer;
template<class T> struct ContextLock {};
struct BufferView {
    std::shared_ptr<Buffer> buffer; u64 size{};
    explicit operator bool() const { return bool(buffer); }
    std::shared_ptr<Buffer> GetBuffer() const { return buffer; }
    u64 GetOffset() const { return 0; }
};
struct Buffer : std::enable_shared_from_this<Buffer> {
    span<u8> bytes;
    BufferView TryGetView(span<u8> requested) {
        auto a=reinterpret_cast<uintptr_t>(requested.data()), b=reinterpret_cast<uintptr_t>(bytes.data());
        return a>=b && a-b<=bytes.size() && requested.size()<=bytes.size()-(a-b) ? BufferView{shared_from_this(),requested.size()} : BufferView{};
    }
};
struct BufferManager {
    unsigned lookups{}; std::vector<span<u8>> lastMappings; u64 lastOffset{};
    BufferView FindOrCreate(span<u8> bytes,u64,auto&&) {
        ++lookups; lastMappings={bytes}; auto buffer=std::make_shared<Buffer>(); buffer->bytes=bytes;
        return {buffer,bytes.size()};
    }
    BufferView FindOrCreateDisjoint(const std::vector<span<u8>> &mappings,u64 offset,u64 size,u64,auto&&) {
        ++lookups; lastMappings=mappings; lastOffset=offset; return {std::make_shared<Buffer>(),size};
    }
    void PrepareBufferAccess(Buffer&,bool=false,u64=0,u64=~u64{}) {}
};
struct InterconnectContext {
    struct AddressSpace { Gmmu gmmu; } addressSpace;
    struct Channel { AddressSpace *asCtx; } channelCtx{&addressSpace};
    struct Gpu { BufferManager buffer; } gpu;
    struct Executor { u64 tag{1}; void AttachLockedBuffer(std::shared_ptr<Buffer>,ContextLock<Buffer>&&){} } executor;
};
#define LOGW(...) ((void)0)
#include "cache.inc"
int main() {
    constexpr u64 Page=constant::PageSize;
    InterconnectContext ctx; CachedMappedBufferView cached;
    alignas(Page) u8 a[Page*2]{}, b[Page]{};
    auto &mm=ctx.addressSpace.gmmu;
    mm.Set(Page,{a,Page}); mm.Set(Page*2,{b,Page});
    cached.Update(ctx,Page,Page*2);
    assert(cached.view && cached.view.size==Page*2); // Previously silently truncated to the first PA span.
    assert(ctx.gpu.buffer.lastMappings.size()==2);
    cached.Update(ctx,Page+17,Page*2-34);
    assert(cached.view.size==Page*2-34 && ctx.gpu.buffer.lastOffset==17);
    mm.Set(Page*2,{nullptr,Page},MemoryMappingKind::Sparse);
    cached.Update(ctx,Page,Page*2);
    assert(cached.view.size==Page*2 && !ctx.gpu.buffer.lastMappings.back().valid());
    mm.Set(Page,{nullptr,Page},MemoryMappingKind::Sparse);
    cached.Update(ctx,Page,Page*2);
    assert(cached.view && cached.view.size==Page*2); // Sparse-only buffers have full logical length.
    mm.Set(Page,{a,Page}); mm.Set(Page*2,{b,Page});
    cached.Update(ctx,Page,Page*2);
    mm.Set(Page*2,{nullptr,Page},MemoryMappingKind::Unmapped);
    cached.Update(ctx,Page,Page*2);
    assert(!cached.view); // Unmapped tail cannot preserve a cached resident prefix.
    mm.Set(Page*2,{a+Page,Page});
    cached.Update(ctx,Page,Page*2);
    assert(cached.view.size==Page*2 && ctx.gpu.buffer.lastMappings.back().data()==a+Page);
    cached.Update(ctx,Page,0);
    assert(!cached.view);
    std::cout << "GPU disjoint cached buffer tests passed\n";
}
