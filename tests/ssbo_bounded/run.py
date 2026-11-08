#!/usr/bin/env python3
"""Run production SSBO binding/mapping bodies with a deterministic host facade.
Does not emulate Vulkan execution, page traps, or concurrent BufferManager operations.
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
gpu = root / 'app/src/main/cpp/skyline/gpu'
p = argparse.ArgumentParser()
p.add_argument('--compiler', type=Path, required=True)
a = p.parse_args()

def function(text, name):
    start = text.index(name)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

pipeline = (gpu / 'interconnect/common/pipeline.inc').read_text()
start = pipeline.index('    struct StorageBufferGuestDescriptor')
end = pipeline.index('    static vk::BufferView GetTextureBufferBinding', start)
methods = pipeline[start:end]
common = (gpu / 'interconnect/common/common.cpp').read_text()
methods += function(common, 'void CachedMappedBufferView::Update(')
methods += function(common, 'void CachedMappedBufferView::PurgeCaches(')
methods += function((gpu / 'buffer.cpp').read_text(), 'BufferBinding BufferView::GetBinding(')
facade = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <tuple>
#include <variant>
#include <vector>
#include <shader_compiler/storage_buffer_range.h>
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
constexpr size_t PAGE_SIZE=4096;
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
namespace vk {
using DeviceSize=u64;
enum class PipelineStageFlagBits:u32 {eVertexShader=8,eComputeShader=2048,eTransfer=4096,eVertexInput=4,eAllCommands=65536};
using PipelineStageFlags=u32;
}
u32 operator|(vk::PipelineStageFlagBits a,vk::PipelineStageFlagBits b){return u32(a)|u32(b);}
u32 operator|(u32 a,vk::PipelineStageFlagBits b){return a|u32(b);}
u32& operator|=(u32&a,vk::PipelineStageFlagBits b){a|=u32(b);return a;}
namespace util {template<class T,class U>T AlignUp(T a,U b){return (a+b-1)&~(T(b)-1);}}
template<class T> struct span {
 T* ptr{}; size_t length{}; T* data()const{return ptr;} size_t size()const{return length;}
 bool empty()const{return !length;} bool valid()const{return ptr;}
 span first(size_t n)const{assert(n<=length);return {ptr,n};}
 span subspan(size_t n)const{assert(n<=length);return {ptr+n,length-n};}
};
struct BufferBinding {int buffer{};u64 offset{},size{};operator bool()const{return buffer;} };
struct GPU; struct Buffer;
template<class T>struct ContextLock {};
struct Delegate {Buffer* buffer{};u64 displacement{};Buffer*GetBuffer(){return buffer;}u64 GetOffset(){return displacement;}};
struct BufferView {
 Delegate*delegate{};u64 offset{},size{};
 operator bool()const{return delegate&&size;}
 Buffer*GetBuffer()const{return delegate->GetBuffer();}
 BufferBinding GetBinding(GPU&)const;
 BufferBinding TryMegaBuffer(auto&,auto&,auto&);
};
using DynamicBufferBinding=std::variant<BufferBinding,BufferView>;
struct Buffer {
 span<u8> physical;Delegate delegate{this,0};int backing=99,dirty{},blocked{},barriers{};bool mega{};
 int GetBacking(){return backing;}
 BufferView TryGetView(span<u8> s){
  if(!s.valid()||s.data()<physical.data()||s.size()>physical.size()||size_t(s.data()-physical.data())>physical.size()-s.size())return {};
  return {&delegate,u64(s.data()-physical.data()),s.size()};
 }
 void PopulateReadBarrier(auto,auto&,auto&){++barriers;}
 bool SequencedCpuBackingWritesBlocked(){return true;}
 void MarkGpuDirty(auto&){++dirty;}
 void BlockSequencedCpuBackingWrites(){++blocked;}
};
BufferBinding BufferView::TryMegaBuffer(auto&,auto&,auto&){return GetBuffer()->mega?BufferBinding{2,128,size}:BufferBinding{};}
struct GPU {
 struct {BufferBinding Allocate(auto&,size_t){return {1,0,PAGE_SIZE};}} megaBufferAllocator;
 struct {u32 minimumStorageBufferAlignment=4;u64 maximumStorageBufferRange=1<<27;} traits;
 struct {std::mutex recreationMutex;Buffer* physical{};int creations{};
  BufferView FindOrCreate(span<u8> s,auto,auto&&){++creations;return physical->TryGetView(s);}
 } buffer;
};
struct Gmmu {
 struct Piece{u64 address;span<u8> mapping;};std::vector<Piece> pieces;int lookups{},translations{};
 auto LookupBlock(u64 address){++lookups;for(auto p:pieces)if(address>=p.address&&address-p.address<p.mapping.size())return std::pair{p.mapping,address-p.address};return std::pair{span<u8>{},u64{0}};}
 auto TranslateRange(u64 address,u64 size){++translations;std::vector<span<u8>> result;
  while(size){auto [m,off]=LookupBlock(address);if(!m.valid())break;auto part=m.subspan(off).first(std::min<u64>(m.size()-off,size));result.push_back(part);address+=part.size();size-=part.size();}return result;}
};
struct AddressSpace{Gmmu gmmu;};
struct InterconnectContext {
 GPU gpu;AddressSpace as;struct{AddressSpace*asCtx;}channelCtx{&as};
 struct {int cycle{},executionTag{},usageTracker{},tag{},attached{};
  void AttachBuffer(auto&){++attached;}void AttachLockedBuffer(auto&,auto&&){}
 }executor;
};
struct ConstantBuffer {
 alignas(16)u8 data[16]{};int reads{},sizedReads{};
 template<class T>T Read(auto&,u32){++reads;sizedReads+=sizeof(T)>8;T t{};std::memcpy(&t,data,sizeof t);return t;}
 void Set(u64 address,u32 size){std::memcpy(data,&address,8);std::memcpy(data+8,&size,4);}
};
struct CachedMappedBufferView {
 BufferView view;span<u8> blockMapping;u64 blockMappingStartAddr{},blockMappingEndAddr{};
 bool lastSplitResolved{};u64 lastSplitAddress{},lastSplitSize{};
 void Update(InterconnectContext&,u64,u64,bool,std::source_location=std::source_location::current());
 void PurgeCaches();
};
'''
cases = r'''
BufferBinding Resolve(DynamicBufferBinding binding,GPU&gpu){if(auto*p=std::get_if<BufferBinding>(&binding))return *p;return std::get<BufferView>(binding).GetBinding(gpu);}
int main(){
 std::vector<u8> storage(16*1024*1024);Buffer physical{{storage.data(),8470528}};
 struct Desc{bool has_size;u32 cbuf_offset;bool is_written;};
 size_t cases{};
 for(bool sized:{false,true})for(bool written:{false,true})for(bool mega:{false,true})
 for(u32 alignment:{4U,256U})for(u64 padding:{0ULL,3ULL})for(u64 extent:{4ULL,4608ULL,196608ULL}){
  InterconnectContext ctx;ctx.gpu.buffer.physical=&physical;ctx.gpu.traits.minimumStorageBufferAlignment=alignment;
  ctx.as.gmmu.pieces={{0x10000000,{storage.data(),storage.size()}}};
  physical.mega=mega;physical.dirty=physical.blocked=physical.barriers=0;
  ConstantBuffer c;c.Set(0x10000000+padding,32768);CachedMappedBufferView cache;u32 src{},dst{};
  Shader::ComputeStorageBufferAccessRange range{.known=true,.constant=extent};
  auto size=range.GetSize({1,1,1},{1,1,1});assert(size==extent);
  auto binding=Resolve(GetStorageBufferBinding(ctx,Desc{sized,0,written},c,cache,vk::PipelineStageFlagBits::eComputeShader,src,dst,size),ctx.gpu);
  const auto expected=util::AlignUp((sized?32768:extent)+padding,alignment);
  assert(binding.size==expected&&binding.size!=physical.physical.size());
  assert(c.sizedReads==int(sized)&&ctx.executor.attached==1&&physical.barriers==1);
  assert(physical.dirty==int(written)&&physical.blocked==int(written||!mega));
  assert(ctx.as.gmmu.translations==0);++cases;
 }
 physical.mega=false;
 InterconnectContext ctx;ctx.gpu.buffer.physical=&physical;
 ctx.as.gmmu.pieces={{0x10000000,{storage.data(),storage.size()}}};ConstantBuffer c;c.Set(0x10000000,4608);
 CachedMappedBufferView cache;u32 src{},dst{};
 auto bind=[&](std::optional<u64> size){return Resolve(GetStorageBufferBinding(ctx,Desc{false,0,false},c,cache,vk::PipelineStageFlagBits::eComputeShader,src,dst,size),ctx.gpu);};
 assert(bind(std::nullopt).size==8388608);assert(c.sizedReads==0);
 assert(bind(4608).size==4608);assert(bind(4).size==4); // cache shrinks and aliases share the same backing
 BufferView overlap{&physical.delegate,256,4608};auto old=overlap.GetBinding(ctx.gpu);
 assert(old.offset==256&&old.size==4608);
 // Simulated delegate successor: actual deferred GetBinding retains logical size and relocation.
 Buffer successor{{storage.data(),storage.size()}};successor.backing=101;
 physical.delegate.buffer=&successor;physical.delegate.displacement=8192;
 auto moved=overlap.GetBinding(ctx.gpu);assert(moved.buffer==101&&moved.offset==8448&&moved.size==4608);
 physical.delegate.buffer=&physical;physical.delegate.displacement=0;
 // Raw pointers stay clamped to first physical mapping; no fabricated contiguous alias.
 cache.PurgeCaches();ctx.as.gmmu.pieces={{0x10000000,{storage.data(),2048}},{0x10000800,{storage.data()+8192,6144}}};
 int translations=ctx.as.gmmu.translations;assert(bind(4608).size==2048);assert(ctx.as.gmmu.translations==translations);
 // Existing sized descriptor path recovers physically adjacent split mappings only.
 cache.PurgeCaches();ctx.as.gmmu.pieces={{0x10000000,{storage.data(),2048}},{0x10000800,{storage.data()+2048,6144}}};c.Set(0x10000000,4608);
 auto sized=Resolve(GetStorageBufferBinding(ctx,Desc{true,0,false},c,cache,vk::PipelineStageFlagBits::eComputeShader,src,dst),ctx.gpu);
 assert(sized.size==4608&&ctx.as.gmmu.translations==translations+1);
 auto counts=ctx.as.gmmu.translations;
 Resolve(GetStorageBufferBinding(ctx,Desc{true,0,false},c,cache,vk::PipelineStageFlagBits::eComputeShader,src,dst),ctx.gpu);assert(ctx.as.gmmu.translations==counts);
 cache.PurgeCaches();ctx.as.gmmu.pieces[1].mapping={storage.data()+8192,6144};
 sized=Resolve(GetStorageBufferBinding(ctx,Desc{true,0,false},c,cache,vk::PipelineStageFlagBits::eComputeShader,src,dst),ctx.gpu);assert(sized.size==2048);
 cache.PurgeCaches();ctx.as.gmmu.pieces.clear();assert(bind(4608).size==PAGE_SIZE&&!cache.view);
 // Cache invalidation in this branch is explicit: callers purge after GMMU
 // remaps. Do not require mapping epochs or StartTextureMapping here; those
 // interfaces are not used by production CachedMappedBufferView::Update.
 // Still exercise P -> Q -> P and switching to another address space.
 InterconnectContext epochCtx;epochCtx.gpu.buffer.physical=&physical;
 epochCtx.as.gmmu.pieces={{0x10000000,{storage.data(),8192}}};
 CachedMappedBufferView epochCache;ConstantBuffer ec;ec.Set(0x10000000,4608);
 auto epochBind=[&](InterconnectContext &e){
  return Resolve(GetStorageBufferBinding(e,Desc{false,0,false},ec,epochCache,
                 vk::PipelineStageFlagBits::eComputeShader,src,dst,u64{4608}),e.gpu);
 };
 auto p=epochBind(epochCtx);assert(p.offset==0&&p.size==4608);
 const auto initialLookups=epochCtx.as.gmmu.lookups;
 auto cached=epochBind(epochCtx);assert(cached.offset==0&&cached.size==4608);
 assert(epochCtx.as.gmmu.lookups==initialLookups); // no lookup on unchanged cached mapping
 epochCtx.as.gmmu.pieces[0].mapping={storage.data()+8192,8192};
 epochCache.PurgeCaches();
 auto q=epochBind(epochCtx);assert(q.offset==8192&&q.size==4608);
 assert(epochCtx.as.gmmu.lookups==initialLookups+1);
 epochCtx.as.gmmu.pieces[0].mapping={storage.data(),8192};
 epochCache.PurgeCaches();
 auto again=epochBind(epochCtx);assert(again.offset==0&&again.size==4608);
 assert(epochCtx.as.gmmu.lookups==initialLookups+2);
 // Reusing the cache with another translator also requires invalidation.
 InterconnectContext other;other.gpu.buffer.physical=&physical;
 other.as.gmmu.pieces={{0x10000000,{storage.data()+16384,8192}}};
 epochCache.PurgeCaches();
 auto newTranslator=epochBind(other);
 assert(newTranslator.offset==16384&&newTranslator.size==4608);
 assert(other.as.gmmu.lookups==1);
 std::cout<<"PASS "<<cases<<" actual binding paths: known/fallback, padding, sized/raw, readonly/write, megabuffer, partial/split mappings, aliases and deferred relocation\n";
}
'''
with tempfile.TemporaryDirectory(prefix='ssbo-host-') as td:
    td=Path(td);src=td/'test.cpp';src.write_text(facade+methods+cases);exe=td/'test'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-pthread','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(a.compiler.resolve()/'include'),str(src),'-o',str(exe)],check=True)
    subprocess.run([exe],check=True,timeout=30)
