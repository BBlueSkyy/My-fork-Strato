#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,tempfile
r=Path(__file__).resolve().parents[2];g=r/'app/src/main/cpp/skyline/gpu'
def section(s):
 a=s.index('    struct StorageBufferGuestDescriptor');b=s.index('    static vk::BufferView GetTextureBufferBinding',a);return s[a:b]
base=subprocess.check_output(['git','show','c7cb28d822c52437ed83e9f86ed15c3a3b77c6e7:app/src/main/cpp/skyline/gpu/interconnect/common/pipeline.inc'],cwd=r,text=True)
a=section(base).replace('GetStorageBufferBinding','GetStorageBufferBindingBase')
b=section((g/'interconnect/common/pipeline.inc').read_text());b=b[b.index('    static DynamicBufferBinding'):]
facade=r'''
#include "gpu/ssbo_range_ab.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <iostream>
using u64=uint64_t;using u32=uint32_t;
namespace ssbo_range_ab=skyline::gpu::ssbo_range_ab;
constexpr size_t PAGE_SIZE=4096;
namespace vk{enum class PipelineStageFlagBits:u32{eVertexShader=8,eComputeShader=2048,eTransfer=4096,eVertexInput=4,eAllCommands=65536};using PipelineStageFlags=u32;}
u32 operator|(vk::PipelineStageFlagBits a,vk::PipelineStageFlagBits b){return u32(a)|u32(b);}u32 operator|(u32 a,vk::PipelineStageFlagBits b){return a|u32(b);}u32&operator|=(u32&a,vk::PipelineStageFlagBits b){a|=u32(b);return a;}
namespace util{template<class T,class U>T AlignUp(T a,U b){return (a+b-1)&~(T(b)-1);}}
struct BufferBinding{int buffer{};u64 offset{},size{};operator bool()const{return buffer;}bool operator==(const BufferBinding&)const=default;};using DynamicBufferBinding=BufferBinding;
struct FakeBuffer{size_t ExperimentGuestSize()const{return 8470528;}int dirty{},blocked{},barriers{};void PopulateReadBarrier(auto,auto&,auto&){++barriers;}bool SequencedCpuBackingWritesBlocked(){return true;}void MarkGpuDirty(auto&){++dirty;}void BlockSequencedCpuBackingWrites(){++blocked;}};
struct FakeView{u64 size{};FakeBuffer buffer{};bool mega{};operator bool()const{return size;}FakeBuffer*GetBuffer(){return &buffer;}BufferBinding TryMegaBuffer(auto&,auto&,auto&){return mega?BufferBinding{2,128,size}:BufferBinding{};}operator BufferBinding()const{return{1,0,size};}};
struct InterconnectContext{struct{struct{BufferBinding Allocate(auto&,size_t){return {1,0,4096};}}megaBufferAllocator;struct{u32 minimumStorageBufferAlignment=4;u64 maximumStorageBufferRange=1<<27;}traits;}gpu;struct{int cycle{},executionTag{},usageTracker{},attached{};void AttachBuffer(auto&){++attached;}}executor;};
struct ConstantBuffer{alignas(16)unsigned char data[16]{};template<class T>T Read(auto&,u32){T t{};std::memcpy(&t,data,sizeof t);return t;}};
struct CachedMappedBufferView{FakeView view{};u64 requested{},address{},available=8388608;bool sized{};void Update(InterconnectContext&,u64 a,u64 s,bool h){requested=s;address=a;sized=h;view.size=std::min(available,s);}u64 RangeAvailableFrom(u64)const{return available;}};
'''
cases=r'''
int main(){
 struct Desc{bool has_size;u32 cbuf_index,cbuf_offset;bool is_written;};
 unsigned cases{};
 for(bool cert:{false,true})for(bool written:{false,true})for(bool sized:{false,true})for(bool mega:{false,true})
 for(u32 alignment:{4U,256U})for(u64 padding:{0ULL,3ULL})for(u32 word:{0U,32U,4608U,32768U})for(u32 off:{0x280U,0x2A0U,0x290U}){
  InterconnectContext ca,cb;ca.gpu.traits.minimumStorageBufferAlignment=cb.gpu.traits.minimumStorageBufferAlignment=alignment;
  ConstantBuffer c;u64 addr=0x10000000+padding;std::memcpy(c.data,&addr,8);std::memcpy(c.data+8,&word,4);
  Desc d{sized,0,off,written};CachedMappedBufferView va,vb;va.view.mega=vb.view.mega=mega;
  std::array<u32,2> rows{ssbo_range_ab::None,ssbo_range_ab::None},indices=rows;u32 sa{},sb{},da{},db{};
  std::optional<u64> access=off==0x2A0?std::optional<u64>{4}:std::nullopt;
  const auto old=GetStorageBufferBindingBase(ca,d,c,va,vk::PipelineStageFlagBits::eComputeShader,sa,da,access);
  const auto now=GetStorageBufferBinding(cb,d,c,vb,vk::PipelineStageFlagBits::eComputeShader,sb,db,access,cert,&rows,&indices,2,{20,12,1});
  const bool changed=STRATO_SSBO_RANGE_AB==1&&cert&&!sized&&!written&&off==0x280;
  if(changed){assert(vb.requested==util::AlignUp(u64{4608}+padding,alignment));assert(now.size>=4608+padding);}
  else{assert(old==now);assert(va.requested==vb.requested&&va.address==vb.address&&va.sized==vb.sized);}
  assert(sa==sb&&da==db&&ca.executor.attached==cb.executor.attached);
  assert(va.view.buffer.dirty==vb.view.buffer.dirty&&va.view.buffer.blocked==vb.view.buffer.blocked&&va.view.buffer.barriers==vb.view.buffer.barriers);
  if(cert&&!sized&&!written&&off!=0x290){unsigned slot=off==0x280?0:1;assert(indices[slot]==2);}
  ++cases;
 }
 auto digest=ssbo_range_ab::ModuleDigest;assert(digest==ssbo_range_ab::ModuleDigest);digest[0]^=1;assert(digest!=ssbo_range_ab::ModuleDigest);
 ssbo_range_ab::Table t;ssbo_range_ab::Key k{.address=1,.slot=0,.legacy=8192};auto i=t.Prepare(k,false);assert(i!=ssbo_range_ab::None);
 t.Resolve(i,4608,8192,8470528,false);assert(t.rows[i].rangeMax==4608&&t.rows[i].backingMax==8470528);
 t.Resolve(i,4608,64,0,true);assert(t.rows[i].mega==1&&t.rows[i].backingMax==8470528);
 std::cout<<"PASS arm="<<STRATO_SSBO_RANGE_AB<<" cases="<<cases<<" original full binding paths, alignment, zero/small legacy words, readonly/write, range vs backing\n";
}
'''
with tempfile.TemporaryDirectory(prefix='ssbo-range-ab-') as td:
 td=Path(td);src=td/'binding.cpp';src.write_text(facade+a+b+cases)
 for arm in [0,1]:
  exe=td/f'arm{arm}';cmd=[os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',f'-DSTRATO_SSBO_RANGE_AB={arm}','-I'+str(g.parent),str(src),'-o',str(exe)]
  subprocess.run(cmd,check=True);subprocess.run([exe],check=True)
 # Counter concurrency and bounded capacity, using the actual helper.
 exe=td/'obs';subprocess.run(cmd[:cmd.index(str(src))]+[str(Path(__file__).with_name('observations.cpp')),'-o',str(exe)],check=True);subprocess.run([exe],check=True)
 # Compile and execute the real deferred GetBinding body with a linked successor.
 def function(text,name):
  a=text.index(name);br=text.index('{',a);e=br+1;depth=1
  while depth:depth+=(text[e]=='{')-(text[e]=='}');e+=1
  return text[a:e]
 body=function((g/'buffer.cpp').read_text(),'BufferBinding BufferView::GetBinding(')
 fixture=r'''
#include <mutex>
#include <optional>
#include <cassert>
#include <cstdint>
struct Span{size_t bytes;size_t size()const{return bytes;}};
struct Buffer{std::optional<Span> guest{Span{8470528}};int GetBacking(){return 99;}};
struct Delegate{Buffer*buffer;uint64_t displacement;Buffer*GetBuffer(){return buffer;}uint64_t GetOffset(){return displacement;}};
struct GPU{struct{std::mutex recreationMutex;}buffer;};
struct BufferBinding{int buffer;uint64_t offset,size;};
struct BufferView{Delegate*delegate;uint64_t offset,size;BufferBinding GetBinding(GPU&,size_t*)const;};
'''+body+r'''
int main(){GPU gpu;Buffer buffer;Delegate successor{&buffer,8192};BufferView view{&successor,256,4608};size_t physical{};
 const auto b=view.GetBinding(gpu,&physical);assert(b.buffer==99&&b.offset==8448&&b.size==4608&&physical==8470528);
 const auto unchanged=view.GetBinding(gpu,nullptr);assert(unchanged.offset==b.offset&&unchanged.size==b.size);
}
'''
 f=td/'resolution.cpp';f.write_text(fixture);exe=td/'resolution';subprocess.run(cmd[:cmd.index(str(src))]+[str(f),'-o',str(exe)],check=True);subprocess.run([exe],check=True)
 print('PASS: actual BufferView::GetBinding with coalesced successor retains offset and view range')
