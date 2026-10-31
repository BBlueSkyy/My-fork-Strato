#!/usr/bin/env python3
"""Host checks of observation and exact binding decisions; no Android/GPU needed."""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]; GPU=ROOT/'app/src/main/cpp/skyline/gpu'
def section(s):
 a=s.index('    struct StorageBufferGuestDescriptor');b=s.index('    static vk::BufferView GetTextureBufferBinding',a);return s[a:b]
with tempfile.TemporaryDirectory(prefix='ssbo-trace-') as td:
 d=Path(td); exe=d/'counters'
 cc=[os.environ.get('CXX','c++'),'-std=c++20','-pthread','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(GPU)]
 subprocess.run(cc+[str(Path(__file__).with_name('counters.cpp')),'-o',str(exe)],check=True);subprocess.run([exe],check=True)
 base=subprocess.check_output(['git','show','b8593d8b2c2e9742a4f1f9edfaa935f653cd3663:app/src/main/cpp/skyline/gpu/interconnect/common/pipeline.inc'],cwd=ROOT,text=True)
 old=section(base).replace('GetStorageBufferBinding','GetStorageBufferBindingBase')
 new=section((GPU/'interconnect/common/pipeline.inc').read_text());new=new[new.index('    static DynamicBufferBinding'):]
 facade=r'''
#include "ssbo_diagnostics.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
using u64=uint64_t;using u32=uint32_t;
namespace ssbo_diagnostics=skyline::gpu::ssbo_diagnostics;
constexpr size_t PAGE_SIZE=4096;
namespace vk {enum class PipelineStageFlagBits:u32{eVertexShader=8,eComputeShader=2048,eTransfer=4096,eVertexInput=4,eAllCommands=65536};using PipelineStageFlags=u32;}
u32 operator|(vk::PipelineStageFlagBits a,vk::PipelineStageFlagBits b){return u32(a)|u32(b);}u32 operator|(u32 a,vk::PipelineStageFlagBits b){return a|u32(b);}u32 &operator|=(u32 &a,vk::PipelineStageFlagBits b){a|=u32(b);return a;}
namespace util {u64 GetTimeNs(){static u64 t;return ++t;}template<class T,class U>T AlignUp(T a,U b){return (a+b-1)&~(T(b)-1);}}
struct BufferBinding{int buffer{};u64 offset{},size{};operator bool()const{return buffer;}bool operator==(const BufferBinding&)const=default;};using DynamicBufferBinding=BufferBinding;
struct FakeBuffer {int dirty{},blocked{},barriers{};bool sequenced{};void PopulateReadBarrier(auto,auto&,auto&){++barriers;}bool SequencedCpuBackingWritesBlocked(){return sequenced;}void MarkGpuDirty(auto&){++dirty;}void BlockSequencedCpuBackingWrites(){++blocked;}size_t DiagnosticId(){return 0;}};
struct FakeView {u64 size{};FakeBuffer buffer{};bool mega{};operator bool()const{return size;}FakeBuffer *GetBuffer(){return &buffer;}u64 GetOffset(){return 0;}BufferBinding TryMegaBuffer(auto&,auto&,auto&){return mega?BufferBinding{2,128,size}:BufferBinding{};}operator BufferBinding()const{return {1,0,size};}};
struct InterconnectContext{struct{struct{BufferBinding Allocate(auto&,size_t){return {1,0,4096};}}megaBufferAllocator;struct{u32 minimumStorageBufferAlignment=256;u64 maximumStorageBufferRange=1<<27;}traits;}gpu;struct{int cycle{},executionTag{},usageTracker{},attached{};void AttachBuffer(auto&){++attached;}}executor;};
struct ConstantBuffer{alignas(16) unsigned char data[16]{};template<class T>T Read(auto&,u32){T t{};std::memcpy(&t,data,sizeof t);return t;}};
struct CachedMappedBufferView{FakeView view{};u64 requested{},address{};bool sized{},missing{};void Update(InterconnectContext&,u64 addr,u64 size,bool hasSize){requested=size;address=addr;sized=hasSize;view.size=missing?0:std::min<u64>(size,4194304);}};
'''
 cases=r'''
int main(){
 ssbo_diagnostics::buffers.Init(0,4194304,0x10000000,false);
 struct Desc {bool has_size;u32 cbuf_index=0,cbuf_offset=0x280;bool is_written;};
 for(bool sized:{false,true})for(bool written:{false,true})for(bool missing:{false,true})for(bool mega:{false,true})for(bool blocked:{false,true})
 for(u64 address:{0ULL,0x10000000ULL,0x10000003ULL})for(u32 size:{0U,4096U,16777216U})for(std::optional<u64> access:std::array<std::optional<u64>,4>{std::nullopt,0,512,134217728}){
  InterconnectContext a,b;ConstantBuffer cb;std::memcpy(cb.data,&address,8);std::memcpy(cb.data+8,&size,4);Desc desc{sized,0,0x280,written};
  CachedMappedBufferView va,vb;va.missing=vb.missing=missing;va.view.mega=vb.view.mega=mega;va.view.buffer.sequenced=vb.view.buffer.sequenced=blocked;
  u32 sa{},sb{},da{},db{};ssbo_diagnostics::Dispatch dispatch{123,456,{4,1,1}};
  auto x=GetStorageBufferBindingBase(a,desc,cb,va,vk::PipelineStageFlagBits::eComputeShader,sa,da,access);
  auto y=GetStorageBufferBinding(b,desc,cb,vb,vk::PipelineStageFlagBits::eComputeShader,sb,db,access,&dispatch);
  assert(x==y && sa==sb && da==db && a.executor.attached==b.executor.attached);
  assert(va.requested==vb.requested && va.address==vb.address && va.sized==vb.sized);
  assert(va.view.buffer.dirty==vb.view.buffer.dirty && va.view.buffer.blocked==vb.view.buffer.blocked && va.view.buffer.barriers==vb.view.buffer.barriers);
 }
}
'''
 p=d/'binding.cpp';p.write_text(facade+old+new+cases);subprocess.run(cc+[str(p),'-o',str(d/'binding')],check=True);subprocess.run([d/'binding'],check=True)
 print('PASS: concurrent counters, identities, overflow, lineage, pre-binding copies; 1152 differential binding cases vs master (UBSan)')
