#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
template<class T> using span=std::span<T>;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught{}; try { f(); } catch (const std::runtime_error &) { caught=true; } check(caught,"expected rejected metadata"); }
std::vector<std::string> events;
#include "intervals.inc"
namespace skyline {
using exception=std::runtime_error;
namespace gpu::interconnect {
struct Mapping { u8 *ptr{}; size_t length{}; bool valid()const{return ptr;} bool empty()const{return !length;} size_t size()const{return length;} u8 *data()const{return ptr;} };
struct Gmmu { std::vector<Mapping> mappings; auto TranslateRange(u64,size_t){return mappings;} };
struct AddressSpace { Gmmu gmmu; };
struct InterconnectContext {
 struct { std::shared_ptr<AddressSpace> asCtx=std::make_shared<AddressSpace>(); } channelCtx;
 struct { struct { IntervalList<u8*> dirtyIntervals,sequencedIntervals; } usageTracker; } executor;
};
}
}
#include "pool.inc"
void testPools() {
 using namespace skyline::gpu::interconnect;
 InterconnectContext ctx; std::array<u32,3> backing{11,22,33};
 auto *bytes=reinterpret_cast<u8*>(backing.data());
 ctx.channelCtx.asCtx->gmmu.mappings={{bytes,5},{bytes+5,7}};
 std::vector<u32> snapshot;
 ReadDynamicPool(ctx,1,3,snapshot); check(snapshot==std::vector<u32>({11,22,33}),"split mapping copies exact pool");
 backing[1]=44; ReadDynamicPool(ctx,1,3,snapshot); check(snapshot[1]==44,"same address metadata write is observed");
 std::array<u32,3> remapped{7,8,9};
 ctx.channelCtx.asCtx->gmmu.mappings={{reinterpret_cast<u8*>(remapped.data()),12}};
 ReadDynamicPool(ctx,1,3,snapshot); check(snapshot[0]==7,"same guest address remap is observed");
 auto *other=reinterpret_cast<u8*>(remapped.data());
 ctx.executor.usageTracker.dirtyIntervals.Insert({other+3,other+4});
 rejects([&]{ReadDynamicPool(ctx,1,3,snapshot);}); ctx.executor.usageTracker.dirtyIntervals.Clear();
 ctx.executor.usageTracker.sequencedIntervals.Insert({other+5,other+6});
 rejects([&]{ReadDynamicPool(ctx,1,3,snapshot);}); ctx.executor.usageTracker.sequencedIntervals.Clear();
 check(snapshot==std::vector<u32>({7,8,9}),"rejection preserves previous snapshot");
 for(auto mappings:std::vector<std::vector<Mapping>>{{},{ {nullptr,12} },{ {other,0} },{ {other,13} },{ {other,11} }}) {
  ctx.channelCtx.asCtx->gmmu.mappings=mappings; rejects([&]{ReadDynamicPool(ctx,1,3,snapshot);});
 }
 rejects([&]{ReadDynamicPool(ctx,0,3,snapshot);}); rejects([&]{ReadDynamicPool(ctx,1,0,snapshot);}); rejects([&]{ReadDynamicPool(ctx,1,(1U<<20)+1,snapshot);});
}
namespace vk {
using PipelineLayout=int; using DescriptorSet=int; using DescriptorSetLayout=int; using PipelineStageFlags=u32;
enum class PipelineBindPoint { eCompute };
struct PipelineStageFlagBits { static constexpr u32 eComputeShader=2,eHost=4; };
struct AccessFlagBits { static constexpr u32 eMemoryWrite=1,eMemoryRead=2,eShaderWrite=4,eHostRead=8; };
struct MemoryBarrier { u32 srcAccessMask{},dstAccessMask{}; };
enum class DescriptorType { eCombinedImageSampler,eSampledImage,eUniformTexelBuffer,eSampler,eStorageBuffer,eUniformBuffer };
using DescriptorSetLayoutCreateFlags=u32;
struct DescriptorSetLayoutCreateFlagBits { static constexpr u32 ePushDescriptorKHR=1; };
struct DescriptorSetLayoutBinding { DescriptorType descriptorType{}; u32 descriptorCount{}; };
struct DescriptorSetLayoutCreateInfo { u32 flags{}; const DescriptorSetLayoutBinding *pBindings{}; u32 bindingCount{}; };
struct PipelineLayoutCreateInfo { const DescriptorSetLayout *pSetLayouts{}; u32 setLayoutCount{}; };
struct ShaderStageFlagBits { static constexpr u32 eCompute=1; };
struct PipelineShaderStageCreateInfo { u32 stage{}; const int *module{}; const char *pName{}; };
struct ComputePipelineCreateInfo { PipelineShaderStageCreateInfo stage; PipelineLayout layout{}; };
std::vector<int> recordedLayouts;
namespace raii {
struct CommandBuffer {
 void bindDescriptorSets(PipelineBindPoint,int layout,u32 index,int set,std::initializer_list<int>) { check(layout==30&&index==1&&set==40,"dynamic descriptor binding ABI"); events.push_back("bind dynamic"); }
 void pipelineBarrier(u32 src,u32 dst,u32,std::initializer_list<MemoryBarrier> barriers,std::initializer_list<int>,std::initializer_list<int>) {
  const auto &barrier=*barriers.begin();
  if (dst==PipelineStageFlagBits::eHost) { check(src==PipelineStageFlagBits::eComputeShader&&barrier.srcAccessMask==AccessFlagBits::eShaderWrite&&barrier.dstAccessMask==AccessFlagBits::eHostRead,"feedback visibility barrier"); events.push_back("host barrier"); }
  else { check(src==8&&dst==16,"propagated texture barrier stages"); events.push_back("read barrier"); }
 }
 void dispatch(u32 x,u32 y,u32 z) { check(x==2&&y==3&&z==4,"dispatch dimensions"); events.push_back("dispatch"); }
};
struct DescriptorSetLayout { int value=10; DescriptorSetLayout(int,const DescriptorSetLayoutCreateInfo &info){check(info.flags==1,"set zero retains push flag");} int operator*()const{return value;} };
struct PipelineLayout { int value=30; PipelineLayout(int,const PipelineLayoutCreateInfo &info){recordedLayouts.assign(info.pSetLayouts,info.pSetLayouts+info.setLayoutCount);} int operator*()const{return value;} };
struct Pipeline { int value=20; Pipeline(int,std::nullptr_t,const ComputePipelineCreateInfo &info){check(info.layout==30,"pipeline uses composed layout");} int operator*()const{return value;} };
}
}
struct FenceCycle {};
struct DescriptorUpdateInfo { int descriptorSetLayout{}; };
struct DescriptorAllocator { struct ActiveDescriptorSet{}; ActiveDescriptorSet AllocateSet(int){return {};} };
struct GPU { int vkDevice{}; DescriptorAllocator descriptor; struct { bool supportsPushDescriptors=true; struct { bool brokenComputeShaders{},brokenMultithreadedPipelineCompilation{}; } quirks; } traits; struct Assembler { void WaitIdle(){events.push_back("wait assembler");} }; std::shared_ptr<Assembler> graphicsPipelineAssembler=std::make_shared<Assembler>(); };
#include "bind.inc"
using BindCachedDescriptorSetCmd=BindCachedDescriptorSetCmdImpl;
struct StateUpdater { std::vector<std::function<void(GPU&,vk::raii::CommandBuffer&)>> commands; void RecordAll(GPU &gpu,vk::raii::CommandBuffer &cmd){for(auto &command:commands)command(gpu,cmd);} };
struct Allocator { template<class T> T *EmplaceUntracked(T t){ static std::unique_ptr<T> value; value=std::make_unique<T>(t); return value.get();} };
struct StateUpdateBuilder {
 StateUpdater state;
 explicit StateUpdateBuilder(Allocator&){}
 template<class T> void AppendCmd(T t){state.commands.push_back([t](GPU &gpu,vk::raii::CommandBuffer &cmd)mutable{t.Record(gpu,cmd);});}
 #include "append.inc"
 void SetPipeline(int,vk::PipelineBindPoint){state.commands.push_back([](auto&,auto&){events.push_back("pipeline");});}
 void SetDescriptorSetWithPush(DescriptorUpdateInfo*){state.commands.push_back([](auto&,auto&){events.push_back("push static");});}
 void SetDescriptorSetWithUpdate(DescriptorUpdateInfo*,DescriptorAllocator::ActiveDescriptorSet*,std::nullptr_t){state.commands.push_back([](auto&,auto&){events.push_back("update static");});}
 StateUpdater Build(){return state;}
};
struct Executor {
 Allocator storage; Allocator *allocator=&storage;
 template<class F> void RunWithTextureBackingRetry(F f){f();}
 template<class T> void AttachDependency(T){}
 void AddCheckpoint(const char*){}
 template<class F> void AddOutsideRpCommand(F f){vk::raii::CommandBuffer cmd; f(cmd,std::shared_ptr<FenceCycle>{},*gpu);}
 GPU *gpu{};
};
struct InterconnectContext { GPU gpu; Executor executor; InterconnectContext(){executor.gpu=&gpu;} };
namespace soc::gm20b::engine::kepler_compute {
struct QMD { enum class SamplerIndex { ViaHeaderIndex,Independent }; static constexpr u32 ConstantBufferCount=1; bool invalidateTextureHeaderCache{},invalidateTextureSamplerCache{}; SamplerIndex samplerIndex{}; u32 ctaRasterWidth=2,ctaRasterHeight=3,ctaRasterDepth=4,programOffset{},constantBufferValid{}; struct Cbuf { u64 Address()const{return 0;} u32 size{}; }; std::array<Cbuf,1> constantBuffer; };
}
using QMD=soc::gm20b::engine::kepler_compute::QMD;
struct Textures { int component{},generation{}; void MarkAllDirty(){events.push_back("invalidate TIC");} };
struct Samplers { void MarkAllDirty(){events.push_back("invalidate TSC");} void Update(InterconnectContext&,bool header){check(header,"QMD sampler mode"); events.push_back("sampler update");} };
struct ConstantBufferSet { struct Entry { int view{}; } entry; Entry &operator[](size_t){return entry;} };
struct DynamicDescriptors {
 std::array<u32,4> limits{};
 void ValidateLimits(InterconnectContext&,u32 a,u32 b,u32 c,u32 d){limits={a,b,c,d};}
 int Layout(){return 50;}
};
using PackedPipelineState=int;
struct ShaderBinary {};
struct Pipeline {
 struct ShaderStage { int value{}; struct Module { int value{}; int &operator*(){return value;} const int &operator*()const{return value;} } module; };
 struct CompiledPipeline { vk::raii::Pipeline pipeline; vk::raii::PipelineLayout pipelineLayout; vk::raii::DescriptorSetLayout descriptorSetLayout; };
 CompiledPipeline compiledPipeline;
 struct { u64 shaderHash{}; int bindlessTextureConstantBufferSlotSelect{}; } sourcePackedState;
 int component,generation; bool dynamic=true,noStatic{};
 Pipeline(InterconnectContext &ctx,Textures &textures,Samplers&,ConstantBufferSet&,int,const ShaderBinary&):compiledPipeline{{ctx.gpu.vkDevice,nullptr,{{},30}},{ctx.gpu.vkDevice,{}},{ctx.gpu.vkDevice,{.flags=1}}},component(textures.component),generation(textures.generation){}
 bool MatchesTextureTypes(InterconnectContext&,Textures &t,ConstantBufferSet&)const{return t.component==component;}
 bool CanBindDynamicDescriptors(InterconnectContext&,Textures &t,Samplers&)const{return t.generation==generation;}
 DescriptorUpdateInfo *SyncDescriptors(InterconnectContext&,ConstantBufferSet&,Samplers&,Textures&,u32&,u32&,std::array<u32,3>){events.push_back("prepare static");static DescriptorUpdateInfo info;return noStatic?nullptr:&info;}
 int SyncDynamicDescriptors(InterconnectContext&,Textures&,Samplers&,u32 &src,u32 &dst){events.push_back("prepare dynamic");if(!dynamic)return 0;src|=8;dst|=16;return 40;}
 bool HasDynamicDescriptors(){return dynamic;}
};
#include "layout.inc"
struct PipelineManager { std::map<int,std::vector<std::unique_ptr<Pipeline>>> map;
#include "variant.inc"
};
struct KeplerCompute {
 InterconnectContext ctx; Textures textures; Samplers samplers;
 struct Buffers { ConstantBufferSet boundConstantBuffers; void Update(InterconnectContext&,const QMD&){events.push_back("cbuf update");} } constantBuffers;
 struct State { Pipeline *pipeline{}; Pipeline *Update(InterconnectContext&,StateUpdateBuilder&,Textures&,Samplers&,ConstantBufferSet&,const QMD&){events.push_back("pipeline update");return pipeline;} } pipelineState;
 struct Registers { struct { struct { u64 programRegion{}; } pipelineStageRegisters; } pipelineStateRegisters; struct Pool { u64 offset{}; u32 maximumIndex{}; }; struct { Pool texHeaderPool; } texturePoolRegisters; struct { Pool texSamplerPool; } samplerPoolRegisters; } diagnosticRegisters;
 void Dispatch(const QMD&);
};
#define LOGW(...) do {} while(false)
#define TRACE_EVENT(...) do {} while(false)
#include "dispatch.inc"
int main() {
 testPools();
 InterconnectContext ctx; Textures textures; Samplers samplers; ConstantBufferSet buffers; ShaderBinary binary;
 std::array<vk::DescriptorSetLayoutBinding,4> bindings{{{vk::DescriptorType::eCombinedImageSampler,2},{vk::DescriptorType::eUniformTexelBuffer,3},{vk::DescriptorType::eStorageBuffer,4},{vk::DescriptorType::eUniformBuffer,1}}};
 DynamicDescriptors descriptors; Pipeline::ShaderStage shader;
 (void)MakeCompiledPipeline(ctx,0,shader,bindings,&descriptors);
 check(vk::recordedLayouts==std::vector<int>({10,50}),"set one dynamic layout appended");
 check(descriptors.limits==std::array<u32,4>{5,2,10,4},"static descriptor counts included in dynamic limits");
 (void)MakeCompiledPipeline(ctx,0,shader,bindings,nullptr);
 check(vk::recordedLayouts==std::vector<int>({10}),"static pipeline has only set zero");
 PipelineManager manager;
 auto *first=manager.FindOrCreate(ctx,textures,samplers,buffers,1,binary);
 textures.generation=1; auto *changed=manager.FindOrCreate(ctx,textures,samplers,buffers,1,binary);
 check(changed!=first,"nonrepresentable pools create variant");
 textures.generation=0; check(manager.FindOrCreate(ctx,textures,samplers,buffers,1,binary)==first,"representable prior pool reuses variant");
 KeplerCompute compute; compute.pipelineState.pipeline=first;
 QMD qmd; qmd.invalidateTextureHeaderCache=qmd.invalidateTextureSamplerCache=true;
 events.clear(); compute.Dispatch(qmd);
 check(events==std::vector<std::string>({"cbuf update","invalidate TIC","invalidate TSC","sampler update","pipeline update","prepare dynamic","prepare static","pipeline","bind dynamic","push static","read barrier","dispatch","host barrier"}),"dispatch invalidation, preparation, binding, texture and feedback barrier order");
 first->dynamic=false; compute.ctx.gpu.traits.supportsPushDescriptors=false;
 events.clear(); compute.Dispatch(qmd);
 check(std::find(events.begin(),events.end(),"host barrier")==events.end()&&std::find(events.begin(),events.end(),"bind dynamic")==events.end(),"static dispatch avoids dynamic binding and barrier");
 check(std::find(events.begin(),events.end(),"update static")!=events.end(),"ordinary set zero allocation remains functional");
 first->noStatic=true;events.clear();compute.Dispatch(qmd);
 check(std::find(events.begin(),events.end(),"update static")==events.end(),"absent set zero does not dereference descriptor info");
 std::cout<<"PASS exact pool reader, cached set recording, compute layout limits, pool variants, QMD invalidations, dispatch binding/barriers\n";
}
