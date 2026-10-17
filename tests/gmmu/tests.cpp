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
#include <limits>
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
#include <unordered_map>
#include <vector>
#include <boost/container/small_vector.hpp>
namespace skyline {
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
namespace constant { inline constexpr size_t PageSize=4096; }
template<class T, size_t Extent=std::dynamic_extent> struct span : std::span<T,Extent> {
    using std::span<T,Extent>::span;
    span(std::span<T,Extent> v):std::span<T,Extent>(v){}
    bool valid() const { return this->data()!=nullptr; }
    bool contains(span other) const {
        const auto start=reinterpret_cast<uintptr_t>(this->data()), address=reinterpret_cast<uintptr_t>(other.data());
        return address>=start && address-start<=this->size() && other.size()<=this->size()-(address-start);
    }
    void copy_from(span other) { std::copy(other.begin(),other.end(),this->begin()); }
};
template<class T> span(T*,size_t)->span<T>;
template<class... T> struct exception : std::runtime_error {
    exception(const char* message,T&&...):std::runtime_error(message){}
};
template<class... T> exception(const char*,T&&...)->exception<T...>;
inline size_t segmentWriteBytes{}; // Test-only accounting at the real SegmentTable::Set boundary.
using SharedSpinLock=std::shared_mutex; // Android lock transport only.
namespace util {
template<class T> constexpr T DivideCeil(T x,T y) { return (x+y-1)/y; }
template<class T,class U> constexpr T AlignDown(T x,U y) { return x&~(y-1); }
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
    mm.MapSparse(Page*2,Page);
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
        else if(mode==1) mm.MapSparse(first*Page,count*Page);
        else mm.Map(first*Page,physical.data()+base*Page,count*Page);
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
void CheckWriteCallback() {
    MM mm; std::vector<u8> bytes(Page), input(Page,0x71);
    mm.Map(Page,bytes.data(),Page);
    unsigned callbacks{};
    mm.Write(Page,span<u8>{input},[&](span<u8> memory) {
        assert(memory.data()==bytes.data() && memory.size()==Page);
        ++callbacks;
    });
    assert(bytes==input && callbacks==1);
}
void CheckSparseCopy() {
    MM mm; std::vector<u8> bytes(Page,0x77);
    mm.Map(Page,bytes.data(),Page);
    mm.MapSparse(Page*2,Page);
    unsigned callbacks{};
    mm.Copy(Page*2,Page,Page,[&](span<u8>) { ++callbacks; });
    assert(callbacks==0); // Discarding a sparse destination must not dereference its placeholder.
    mm.Copy(Page,Page*2,Page,[&](span<u8> memory) { assert(memory.data()==bytes.data()); ++callbacks; });
    assert(callbacks==1 && std::all_of(bytes.begin(),bytes.end(),[](auto b){return b==0;}));
}
void CheckUnmappedIo() {
    MM mm; std::array<u8,16> bytes{};
    bool fault{};
    try { mm.Read(bytes.data(),0,bytes.size()); } catch(const std::runtime_error&) { fault=true; }
    assert(fault);
    fault=false;
    try { mm.Write(0,bytes.data(),bytes.size()); } catch(const std::runtime_error&) { fault=true; }
    assert(fault);
}
void CheckFragmentedIo() {
    MM mm;
    std::vector<u8> a(Page,0x12), b(Page,0x34), destination(Page*3,0xEF), input(Page*3,0xA5);
    mm.Map(Page,a.data(),Page);
    mm.MapSparse(Page*2,Page);
    mm.Map(Page*3,b.data(),Page);
    std::vector<u8> expected; expected.insert(expected.end(),a.begin(),a.end());
    expected.resize(Page*2,0); expected.insert(expected.end(),b.begin(),b.end());
    unsigned accesses{};
    auto callback=[&](span<u8> memory) { assert(memory.valid() && memory.size()<=Page); ++accesses; };
    mm.Read(destination.data(),Page,destination.size(),callback);
    assert(destination==expected && accesses==2);
    accesses=0;
    auto got=mm.ReadTill(destination,Page,[](span<u8>)->std::optional<size_t>{return std::nullopt;},callback);
    assert(got.size()==Page*3 && accesses==2 && destination==expected);
    destination.resize(Page*4);
    got=mm.ReadTill(destination,Page,[](span<u8>)->std::optional<size_t>{return std::nullopt;});
    assert(got.size()==Page*3); // Unmapped tail stops ReadTill, never walks past blocks.end().
    accesses=0;
    mm.Write(Page,span<u8>{input},callback);
    assert(accesses==2 && a==b && a.front()==0xA5);
    mm.Read(destination.data(),Page,Page*3);
    assert(std::all_of(destination.begin()+Page,destination.begin()+Page*2,[](auto b){return b==0;}));
    bool fault{};
    try { mm.Read(destination.data(),Page,Page*4); } catch(const std::runtime_error&) { fault=true; }
    assert(fault);
    fault=false;
    try { mm.ReadTill(destination,Page,[](span<u8> s)->std::optional<size_t>{return s.size()+1;}); }
    catch(const std::runtime_error&) { fault=true; }
    assert(fault);
    mm.Read(nullptr,0,0); mm.Write(0,static_cast<u8 *>(nullptr),u64{0}); mm.Copy(0,0,0);
    fault=false;
    try { mm.Copy(Page,MM::VaMaximum-4,16); } catch(const std::runtime_error&) { fault=true; }
    assert(fault);
    // Independent source/destination boundaries: resident + sparse sources,
    // and resident + sparse destinations, without scratch copies.
    std::vector<u8> output(Page*3,0x99);
    mm.Map(Page*8,output.data(),Page);
    mm.MapSparse(Page*9,Page);
    mm.Map(Page*10,output.data()+Page*2,Page);
    mm.Copy(Page*8+17,Page+17,Page*3-34);
    assert(output[16]==0x99 && output[17]==0xA5 && output[Page*2]==0xA5 && output.back()==0x99);
}
void CheckSparseRemapWork() {
    auto update=[](u64 reservation) {
        FlatMemoryManager<u64,0,40,12,17> mm;
        std::vector<u8> page(Page,0x51);
        mm.MapSparse(Page,reservation);
        segmentWriteBytes=0;
        mm.Map(Page*3,page.data(),Page);
        const auto work=segmentWriteBytes;
        auto ranges=mm.TranslateMappings(Page*2,Page*3);
        assert(ranges.size()==3 && ranges[0].kind==MemoryMappingKind::Sparse &&
            ranges[1].memory.data()==page.data() && ranges[2].kind==MemoryMappingKind::Sparse);
        return work;
    };
    const auto small=update(1ULL<<24), large=update(1ULL<<34);
    assert(large==small && large<=Page*4); // Work must not scale with the untouched sparse reservation.
}
// Compile both production specializations, including 32-bit host1x endpoints.
namespace skyline {
template class FlatMemoryManager<u64,0,40,12,17>;
template class FlatMemoryManager<u32,0,32,12,17>;
}
namespace tsl { template<class K,class V,class H=std::hash<K>> using robin_map=std::unordered_map<K,V,H>; }
namespace skyline::gpu::interconnect {
    struct Buffer;
    template<class T> struct ContextLock {};
    struct BufferView {
        std::shared_ptr<Buffer> buffer;
        span<u8> memory;
        size_t size{};
        BufferView() = default;
        BufferView(std::shared_ptr<Buffer> buffer, span<u8> memory) : buffer{std::move(buffer)}, memory{memory}, size{memory.size()} {}
        size_t GetOffset() const { return 0; }
        explicit operator bool() const { return bool(buffer); }
        std::shared_ptr<Buffer> GetBuffer() const { return buffer; }
    };
    struct Buffer : std::enable_shared_from_this<Buffer> {
        span<u8> memory;
        std::vector<u8> owned;
        BufferView TryGetView(span<u8> requested) {
            return requested.data()>=memory.data() && requested.data()+requested.size()<=memory.data()+memory.size() ?
                BufferView{shared_from_this(),requested} : BufferView{};
        }
    };
    struct BufferManager {
        unsigned lookups{};
        void PrepareBufferAccess(Buffer&, bool=false, u64=0, u64=std::numeric_limits<u64>::max()) {}
        BufferView FindOrCreateDisjoint(const std::vector<span<u8>> &, size_t offset, size_t size, u64, auto&&) {
            auto buffer=std::make_shared<Buffer>(); buffer->owned.resize(offset+size); buffer->memory=span<u8>{buffer->owned};
            return {buffer, buffer->memory.subspan(offset,size)};
        }
        BufferView FindOrCreate(span<u8> memory,u64,auto&&) {
            ++lookups;
            auto buffer=std::make_shared<Buffer>(); buffer->memory=memory;
            return {buffer,memory};
        }
    };
    struct InterconnectContext {
        struct AddressSpace { MM gmmu; } addressSpace;
        struct Channel { AddressSpace *asCtx; } channelCtx{&addressSpace};
        struct Gpu { BufferManager buffer; } gpu;
        struct Memory { span<u8> CreateMirror(span<u8> mapping) { return mapping; } } memory;
        struct Trap {
            struct Entry { span<u8> memory; std::function<bool()> write; };
            std::vector<Entry> entries;
            u64 CreateTrap(span<u8> memory, auto&&, auto&&, auto&& write) {
                entries.push_back({memory,write}); return entries.size();
            }
            void TrapRegions(u64,bool) {}
            void Write(u8* address) {
                for(auto &entry:entries)
                    if(address>=entry.memory.data() && address<entry.memory.data()+entry.memory.size()) assert(entry.write());
            }
        } trap;
        struct Executor { u64 tag{1}; void AttachLockedBuffer(std::shared_ptr<Buffer>,ContextLock<Buffer>&&){} } executor;
    };
    struct CommandExecutor {
        u64 mappingGeneration{};
        std::vector<std::function<void()>> flushCallbacks;
        void Submit() = delete; // Mapping invalidation must never submit GPU work.
        void NotifyMemoryMappingChange(u64);
    };
    #include "mapping_notify.inc"
    void CheckMappingNotification() {
        CommandExecutor executor;
        unsigned flushes{};
        executor.flushCallbacks.push_back([&]{++flushes;});
        executor.NotifyMemoryMappingChange(1);
        executor.NotifyMemoryMappingChange(1);
        assert(flushes==1);
        executor.NotifyMemoryMappingChange(2);
        assert(flushes==2);
    }
    using ContextTag=u64; using TrapHandle=u64;
    struct ShaderBinary { span<u8> binary; u32 baseOffset; };
    #include "shader_class.inc"
    #include "shader_select.inc"
    void CheckShaderMirrorGrowth() {
        InterconnectContext ctx;
        std::vector<u8> bytes(Page*2,0x31);
        ctx.addressSpace.gmmu.Map(Page,bytes.data(),Page);
        ShaderCache cache;
        cache.SelectMirrorForTest(ctx,ctx.addressSpace.gmmu.LookupBlock(Page).first);
        const auto old=cache.entry->mirror;
        ctx.addressSpace.gmmu.Map(Page*2,bytes.data()+Page,Page);
        cache.SelectMirrorForTest(ctx,ctx.addressSpace.gmmu.LookupBlock(Page*2).first);
        assert(cache.entry->mirror.size()>=Page*2 && old.size()==Page && old[0]==0x31);
        cache.entry->dirty=false;
        bytes[Page]=0x42;
        ctx.trap.Write(bytes.data()+Page);
        cache.SelectMirrorForTest(ctx,ctx.addressSpace.gmmu.LookupBlock(Page*2).first);
        assert(cache.entry->mirror[Page]==0x42 && cache.entry->dirty); // The enlarged region needs its own write trap.
    }
    unsigned splitWarnings{};
    #define LOGW(...) (++splitWarnings)
    #include "buffer_class.inc"
    #include "buffer_update.inc"
    void CheckCachedRemap() {
        InterconnectContext ctx;
        std::vector<u8> a(Page*2), b(Page), tail(Page);
        auto &mm=ctx.addressSpace.gmmu;
        mm.Map(Page,a.data(),Page);
        mm.Map(Page*2,a.data()+Page,Page);
        CachedMappedBufferView cached{};
        cached.Update(ctx,Page,Page*2);
        assert(cached.view && cached.view.memory.size()==Page*2 && splitWarnings==0);
        const auto lookups=ctx.gpu.buffer.lookups;
        cached.Update(ctx,Page,Page);
        assert(ctx.gpu.buffer.lookups==lookups); // Unchanged physical backing reuses BufferManager views.
        mm.Map(Page,b.data(),Page);
        cached.Update(ctx,Page,Page);
        assert(cached.view.memory.data()==b.data()); // Same IOVA must not preserve an obsolete PA.
        cached.Update(ctx,Page,Page*2);
        assert(cached.view.size==Page*2 && splitWarnings==0); // A logical view covers every physical fragment.
        mm.Unmap(Page,Page);
        cached.Update(ctx,Page,Page);
        assert(!cached.view);
        mm.MapSparse(Page,Page);
        cached.Update(ctx,Page,Page);
        assert(cached.view && cached.view.size == Page);
        mm.Map(Page,tail.data(),Page);
        cached.Update(ctx,Page,Page);
        assert(cached.view.memory.data()==tail.data());
    }
}
namespace skyline::gpu {
    struct GuestTexture {
        std::vector<span<u8>> mappings;
        std::vector<bool> sparseMappings;
        size_t mappingOffset{}, logicalSize{Page * 3 - 32};
        size_t GetSize() const { return logicalSize; }
        bool MappingsValid() const;
        #include "guest_set.inc"
    };
    #include "guest_valid.inc"
    void CheckTypedSparseTextures() {
        MM mm; std::vector<u8> first(Page), last(Page);
        mm.Map(Page, first.data(), Page);
        mm.MapSparse(Page * 2, Page);
        mm.Map(Page * 3, last.data(), Page);
        GuestTexture texture;
        texture.SetMappings(mm, Page + 32);
        assert(texture.MappingsValid() && texture.mappingOffset == 32);
        assert(texture.mappings.size() == 3 && texture.mappings.front().data() == first.data() + 32);
        assert(texture.mappings.front().size() == Page - 32 && texture.mappings.back().data() == last.data());
        assert(!texture.sparseMappings[0] && texture.sparseMappings[1] && !texture.sparseMappings[2]);
        mm.Unmap(Page * 2, Page);
        texture.SetMappings(mm, Page + 32);
        assert(!texture.MappingsValid() && !texture.sparseMappings[1]);
        mm.Map(Page * 2, first.data(), Page);
        texture.SetMappings(mm, Page + 32);
        assert(texture.MappingsValid() && texture.mappings[1].data() == first.data());
        mm.MapSparse(Page, Page * 3);
        texture.SetMappings(mm, Page + 32);
        assert(texture.MappingsValid() && texture.sparseMappings.front());
    }
    struct UsageTracker { struct Intervals {
        unsigned writes{}; void Insert(span<u8> memory) { assert(memory.valid()); ++writes; }
    } dirtyIntervals; };
    struct Texture {
        struct Guest { std::vector<span<u8>> mappings; };
        std::optional<Guest> guest;
        void RecordGpuWrite() {} // Alias publication/transport remains outside this GMMU integration test.
        void MarkGpuDirty(UsageTracker&);
    };
    #include "mark_dirty.inc"
    void CheckHostOnlyFallback() {
        Texture texture; UsageTracker tracker;
        texture.MarkGpuDirty(tracker);
        assert(tracker.dirtyIntervals.writes==0);
        std::array<u8,16> memory{};
        texture.guest.emplace(Texture::Guest{{span<u8>{memory}}});
        texture.MarkGpuDirty(tracker);
        assert(tracker.dirtyIntervals.writes==1);
    }
}
int main(int argc,char**argv) {
    std::string name=argc>1?argv[1]:"all";
    if(name=="sparse" || name=="all") { CheckSparse(); skyline::gpu::CheckTypedSparseTextures(); }
    if(name=="remap" || name=="all") CheckRemap();
    if(name=="l2" || name=="all") CheckL2Tail();
    if(name=="cache" || name=="all") skyline::gpu::interconnect::CheckCachedRemap();
    if(name=="host" || name=="all") skyline::gpu::CheckHostOnlyFallback();
    if(name=="notify" || name=="all") skyline::gpu::interconnect::CheckMappingNotification();
    if(name=="work" || name=="all") CheckSparseRemapWork();
    if(name=="shader" || name=="all") skyline::gpu::interconnect::CheckShaderMirrorGrowth();
    if(name=="copy") CheckSparseCopy();
    if(name=="fault") CheckUnmappedIo();
    if(name=="io" || name=="all") { CheckWriteCallback(); CheckSparseCopy(); CheckUnmappedIo(); CheckFragmentedIo(); }
    if(name=="all") CheckEndpoints();
    if(name=="random" || name=="all") CheckRandomMappings();
    std::cout<<"PASS: "<<name<<'\n';
}
