// SPDX-License-Identifier: MPL-2.0
// Execute the complete production runtime with a validating descriptor device.
// Real Vulkan signatures are checked separately by native_compile.cpp.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
static void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
namespace fixture {
inline u64 next{1}; inline unsigned writes{}, copies{}, faults{}, preparations{}, buffers{};
inline std::set<u64> pools;
inline std::map<u64,u64> setPools;
inline std::map<std::tuple<u64,u32,u32>,u64> descriptors;
inline std::map<std::tuple<u64,u32,u32>,int> imageLayouts;
inline int lastLayoutStage{}, lastReadStage{};
inline std::vector<u32*> metadata;
}
namespace vk {
template<class Tag> struct Handle { u64 value{}; Handle() = default; Handle(std::nullptr_t) {} explicit Handle(u64 v):value(v){} explicit operator bool()const{return value;} bool operator==(const Handle&)const=default; };
using DescriptorSet = Handle<struct S>; using DescriptorSetLayout = Handle<struct L>; using DescriptorPool = Handle<struct P>;
using ImageView = Handle<struct I>; using Sampler = Handle<struct A>; using Buffer = Handle<struct B>;
using DeviceSize = u64; using PipelineStageFlags = u32;
enum class DescriptorType { eSampler, eSampledImage, eStorageBuffer };
enum class ShaderStageFlagBits { eCompute, eFragment }; using ShaderStageFlags = ShaderStageFlagBits;
enum class PipelineStageFlagBits { eComputeShader, eAllGraphics };
enum class ImageLayout { eUndefined, eGeneral };
enum class DescriptorBindingFlagBits { ePartiallyBound }; using DescriptorBindingFlags = DescriptorBindingFlagBits;
enum class ImageUsageFlagBits { eSampled = 1 }; using ImageUsageFlags = ImageUsageFlagBits;
inline u32 operator&(ImageUsageFlags lhs, ImageUsageFlags rhs){return u32(lhs)&u32(rhs);}
enum class Result { eSuccess }; inline const char *to_string(Result){return "success";}
struct PhysicalDeviceLimits { u32 maxPerStageDescriptorSampledImages{4096},maxDescriptorSetSampledImages{4096},maxPerStageDescriptorSamplers{4096},maxDescriptorSetSamplers{4096},maxPerStageResources{16384},maxBoundDescriptorSets{8},maxPerStageDescriptorStorageBuffers{64},maxDescriptorSetStorageBuffers{64}; };
struct DescriptorPoolSize { DescriptorType type; u32 descriptorCount; };
struct DescriptorPoolCreateInfo { u32 maxSets,poolSizeCount; const DescriptorPoolSize *pPoolSizes; };
struct DescriptorSetAllocateInfo { DescriptorPool descriptorPool; u32 descriptorSetCount; const DescriptorSetLayout *pSetLayouts; };
struct DescriptorBufferInfo { Buffer buffer; DeviceSize offset,range; };
struct DescriptorImageInfo { Sampler sampler; ImageView imageView; ImageLayout imageLayout; };
struct WriteDescriptorSet { DescriptorSet dstSet; u32 dstBinding{},dstArrayElement{},descriptorCount{}; DescriptorType descriptorType; const DescriptorImageInfo *pImageInfo{}; const DescriptorBufferInfo *pBufferInfo{}; };
struct CopyDescriptorSet { DescriptorSet srcSet; u32 srcBinding{},srcArrayElement{}; DescriptorSet dstSet; u32 dstBinding{},dstArrayElement{},descriptorCount{}; };
struct DescriptorSetLayoutBinding {u32 binding;DescriptorType descriptorType;u32 descriptorCount;ShaderStageFlagBits stageFlags;};
struct DescriptorSetLayoutBindingFlagsCreateInfo { u32 bindingCount; const DescriptorBindingFlags *pBindingFlags; };
struct DescriptorSetLayoutCreateInfo { const void *pNext;u32 bindingCount;const DescriptorSetLayoutBinding *pBindings;};
namespace raii {
struct Device {
 struct Raw { Result allocateDescriptorSets(const DescriptorSetAllocateInfo *i,DescriptorSet *set,int)const { Check(fixture::pools.contains(i->descriptorPool.value),"allocate from dead pool"); *set=DescriptorSet(fixture::next++);fixture::setPools[set->value]=i->descriptorPool.value;return Result::eSuccess;} } raw;
 const Raw &operator*()const{return raw;} const int *getDispatcher()const{static int d{};return &d;}
 void updateDescriptorSets(WriteDescriptorSet write,std::initializer_list<int>)const {
  Check(fixture::pools.contains(fixture::setPools.at(write.dstSet.value)),"write dead set");
  for(u32 i{};i<write.descriptorCount;++i){u64 value{}; if(write.pBufferInfo)value=write.pBufferInfo[i].buffer.value;else value=write.descriptorType==DescriptorType::eSampler?write.pImageInfo[i].sampler.value:write.pImageInfo[i].imageView.value;Check(value,"write null descriptor");fixture::descriptors[{write.dstSet.value,write.dstBinding,write.dstArrayElement+i}]=value;if(write.descriptorType==DescriptorType::eSampledImage)fixture::imageLayouts[{write.dstSet.value,write.dstBinding,write.dstArrayElement+i}]=int(write.pImageInfo[i].imageLayout);}++fixture::writes;
 }
 void updateDescriptorSets(std::initializer_list<int>,CopyDescriptorSet copy)const {
  Check(fixture::pools.contains(fixture::setPools.at(copy.srcSet.value)),"copy dead source");
  for(u32 i{};i<copy.descriptorCount;++i) {auto source=fixture::descriptors.find({copy.srcSet.value,copy.srcBinding,copy.srcArrayElement+i});Check(source!=fixture::descriptors.end(),"copy uninitialized descriptor");fixture::descriptors[{copy.dstSet.value,copy.dstBinding,copy.dstArrayElement+i}]=source->second;auto layout=fixture::imageLayouts.find({copy.srcSet.value,copy.srcBinding,copy.srcArrayElement+i});if(layout!=fixture::imageLayouts.end())fixture::imageLayouts[{copy.dstSet.value,copy.dstBinding,copy.dstArrayElement+i}]=layout->second;}++fixture::copies;
 }
};
struct DescriptorPool { vk::DescriptorPool h; DescriptorPool(std::nullptr_t){} DescriptorPool(const Device&,const DescriptorPoolCreateInfo &):h(fixture::next++){fixture::pools.insert(h.value);} DescriptorPool(DescriptorPool&&o):h(std::exchange(o.h,{})){} DescriptorPool&operator=(DescriptorPool&&o){if(h)fixture::pools.erase(h.value);h=std::exchange(o.h,{});return *this;}~DescriptorPool(){if(h)fixture::pools.erase(h.value);}vk::DescriptorPool operator*()const{return h;} };
struct DescriptorSetLayout {vk::DescriptorSetLayout h;DescriptorSetLayout(std::nullptr_t){}DescriptorSetLayout(const Device&,const DescriptorSetLayoutCreateInfo &info):h(fixture::next++){for(u32 i{};i<info.bindingCount;++i){fixture::lastLayoutStage=int(info.pBindings[i].stageFlags);Check(info.pBindings[i].stageFlags==info.pBindings[0].stageFlags,"stage mask differs across dynamic bindings");}}DescriptorSetLayout&operator=(DescriptorSetLayout&&)=default;vk::DescriptorSetLayout operator*()const{return h;}};
struct Sampler {vk::Sampler h;explicit Sampler(u64 v):h(v){} vk::Sampler operator*()const{return h;}};
}
}
namespace Shader {enum class TextureType{Color2D,Buffer};enum class TextureComponentType{Float,Uint,Sint};struct DynamicTextureDescriptor{TextureType type;bool is_depth;bool is_samplerless;};}
#define LOGE(...) do { ++fixture::faults; } while(false)
namespace skyline {
template<class T>using span=std::span<T>;
struct exception:std::runtime_error{template<class...Args>exception(const char*s,Args&&...):std::runtime_error(s){}};
namespace gpu {
namespace feedback { inline constexpr vk::ImageLayout Layout{static_cast<vk::ImageLayout>(1000339000)}; }
struct FenceCycle {bool completed{};std::vector<std::shared_ptr<void>> dependencies;bool Poll(bool,bool destroy){if(completed&&destroy)dependencies.clear();return completed;} };
namespace memory {
struct Buffer {vk::Buffer vkBuffer;std::unique_ptr<u8[]> bytes;size_t length;Buffer(size_t n):vkBuffer(fixture::next++),bytes(new u8[n]),length(n){++fixture::buffers;fixture::metadata.push_back(reinterpret_cast<u32*>(bytes.get()));}Buffer(Buffer&&o):vkBuffer(std::exchange(o.vkBuffer,{})),bytes(std::move(o.bytes)),length(o.length){}~Buffer(){if(vkBuffer)--fixture::buffers;}u8*data(){return bytes.get();}};
struct MemoryManager {Buffer AllocateBuffer(size_t n){return Buffer(n);}};
}
struct GPU {vk::raii::Device vkDevice;memory::MemoryManager memory;struct{u32 maximumStorageBufferRange{1<<20};vk::PhysicalDeviceLimits dynamicDescriptorLimits;}traits;};
struct Texture {u64 backingGeneration{1};u64 GetBackingGeneration() const { return backingGeneration; }vk::ImageLayout layout{};unsigned transitions{};void TransitionLayout(vk::ImageLayout target){layout=target;++transitions;}void PopulateReadBarrier(vk::PipelineStageFlagBits stage,vk::PipelineStageFlags&s,vk::PipelineStageFlags&d){fixture::lastReadStage=int(stage);s|=1;d|=1;}};
struct TextureView:std::enable_shared_from_this<TextureView>{std::shared_ptr<Texture> texture=std::make_shared<Texture>();vk::ImageView image;explicit TextureView(u64 v):image(v){}vk::ImageUsageFlags GetUsage(){return vk::ImageUsageFlagBits::eSampled;}vk::ImageView GetView(){return image;}};
namespace interconnect {
struct CommandExecutor {std::shared_ptr<FenceCycle>cycle=std::make_shared<FenceCycle>();void AttachTexture(TextureView*){++fixture::preparations;}void AttachDependency(const std::shared_ptr<void>&p){cycle->dependencies.push_back(p);}};
struct InterconnectContext{GPU&gpu;CommandExecutor&executor;};
struct Textures {bool supportsLinear{};std::vector<u32>headers{1,2,3};std::vector<std::shared_ptr<TextureView>>views{std::make_shared<TextureView>(101),std::make_shared<TextureView>(102),std::make_shared<TextureView>(103)};span<const u32>GetPoolHeaders(InterconnectContext&){return headers;}TextureView*GetDynamicTexture(InterconnectContext&,u32 i,Shader::TextureType){return views[i].get();}bool DynamicTextureSupportsLinear(InterconnectContext&,u32 i){return supportsLinear&&i==0;}Shader::TextureComponentType GetDynamicTextureComponentType(u32 i){return static_cast<Shader::TextureComponentType>(i%3);}};
struct Samplers {std::vector<u32>headers{1,0,3};bool viaHeader{},linear{};std::shared_ptr<vk::raii::Sampler> first=std::make_shared<vk::raii::Sampler>(201),last=std::make_shared<vk::raii::Sampler>(203);span<const u32>GetPoolHeaders(InterconnectContext&){return headers;}bool IsViaHeaderBinding(){return viaHeader;}bool DynamicSamplerUsesLinear(u32 i){return linear&&i==0;}std::shared_ptr<vk::raii::Sampler>GetDynamicSampler(InterconnectContext&,u32 i){return headers[i]?(i?last:first):nullptr;}};
}
}
}
#include "header.inc"
#include "source.inc"
int main(){
 using namespace skyline::gpu;using namespace interconnect;
 GPU gpu;CommandExecutor executor;InterconnectContext ctx{gpu,executor};Textures textures;Samplers samplers;
 const std::array groups{Shader::DynamicTextureDescriptor{Shader::TextureType::Color2D,false,false}};
 vk::PipelineStageFlags src{},dst{};std::weak_ptr<vk::raii::Sampler> retainedSampler=samplers.first;
 std::weak_ptr<TextureView> retainedView=textures.views[0];
 {
  DynamicDescriptors tables(ctx,textures,samplers,groups);
  auto first=tables.Sync(ctx,textures,samplers,src,dst);unsigned firstWrites=fixture::writes;
  auto words=fixture::metadata.back();Check(words[0]==3&&words[1]==3&&words[5]==1,"metadata dimensions");Check(words[9]==1&&words[10]==0&&words[11]==1,"invalid sampler hole");Check(words[12]==0&&words[13]==1&&words[14]==2,"typed compact image maps");
  Check(tables.Sync(ctx,textures,samplers,src,dst)==first,"unchanged dispatch allocated new set");Check(fixture::writes==firstWrites,"unchanged dispatch rewrote descriptors");Check(fixture::preparations==6,"cached set skipped resource preparation");for(const auto &view:textures.views)Check(view->texture->transitions==1,"unchanged dynamic candidate repeated blocking layout transition");
  textures.views[1]->image=vk::ImageView(202);
  auto changed=tables.Sync(ctx,textures,samplers,src,dst);Check(changed!=first,"changed backing mutated live set");Check(fixture::writes==firstWrites+2,"changed snapshot should write only metadata and changed image");Check(fixture::copies>=3,"unchanged initialized entries not copied");
  Check(fixture::descriptors.at({first.value,3,0})==102,"old in-flight descriptor changed");Check(fixture::descriptors.at({changed.value,3,0})==202,"new view not published");
  ++textures.views[1]->texture->backingGeneration;unsigned beforeGeneration=fixture::writes;
  auto generationSet=tables.Sync(ctx,textures,samplers,src,dst);Check(generationSet!=changed,"backing generation lost when Vulkan handle reused");
  Check(fixture::writes==beforeGeneration+2,"recycled handle must rewrite its descriptor");
  changed=generationSet;
  auto oldCycle=executor.cycle;executor.cycle=std::make_shared<FenceCycle>();
  auto concurrent=tables.Sync(ctx,textures,samplers,src,dst);Check(concurrent!=changed,"another pending cycle reused writable fault metadata");
  oldCycle->completed=true;oldCycle->Poll(false,true);
  words=fixture::metadata.back();words[3]=7;words[4]=0xDEADBEEF;
  auto completedCycle=executor.cycle;completedCycle->completed=true;completedCycle->Poll(false,true);Check(fixture::faults==1,"fault not reported on fence completion");
  executor.cycle=std::make_shared<FenceCycle>();auto recycled=tables.Sync(ctx,textures,samplers,src,dst);Check(recycled==concurrent,"completed unchanged snapshot not recycled");Check(words[3]==0&&words[4]==0,"recycled fault metadata not reset");
  samplers.viaHeader=true;auto headerSet=tables.Sync(ctx,textures,samplers,src,dst);Check(headerSet!=recycled,"header binding mode did not invalidate metadata");Check(fixture::metadata.back()[2]==1,"header binding mode missing");
  samplers.linear=true;textures.supportsLinear=true;unsigned beforeFlags=fixture::writes;
  auto flagsSet=tables.Sync(ctx,textures,samplers,src,dst);Check(flagsSet!=headerSet,"filter flags did not invalidate metadata");
  Check(fixture::writes==beforeFlags+1,"filter metadata update rewrote resource descriptors");
  auto flagsWords=fixture::metadata.back();Check(flagsWords[9]==3&&(flagsWords[12]&0x80000000u),"linear pairing capability flags missing");
  bool limitRejected{};try{tables.ValidateLimits(ctx,0,0,0,64);}catch(const std::exception&){limitRejected=true;}Check(limitRejected,"metadata storage descriptor omitted from limits");
  textures.headers.push_back(4);textures.views.push_back(std::make_shared<TextureView>(104));Check(!tables.CanRepresent(ctx,textures,samplers),"capacity growth did not request pipeline variant");
 }
 samplers.first.reset();samplers.last.reset();Check(!retainedSampler.expired(),"in-flight sampler freed with sampler owner");
 textures.views.clear();Check(!retainedView.expired(),"in-flight image view freed with texture owner");
 Check(fixture::buffers>0,"in-flight metadata freed with pipeline");Check(!fixture::pools.empty(),"in-flight descriptor pools freed with pipeline");
 executor.cycle->completed=true;executor.cycle->Poll(false,true);Check(fixture::buffers==0&&fixture::pools.empty(),"snapshot resources leaked after completion");Check(retainedSampler.expired(),"sampler leaked after fence completion");Check(retainedView.expired(),"image view leaked after fence completion");
 executor.cycle=std::make_shared<FenceCycle>();
 {
  Textures graphicsTextures;Samplers graphicsSamplers;
  DynamicDescriptors tables(ctx,graphicsTextures,graphicsSamplers,groups,vk::ShaderStageFlagBits::eFragment);
  Check(fixture::lastLayoutStage==int(vk::ShaderStageFlagBits::eFragment),"graphics descriptor visibility retained compute stage");
  std::vector<TextureView*> candidates;
  tables.Prepare(ctx,graphicsTextures,graphicsSamplers,src,dst,candidates);
  Check(candidates.size()==3&&candidates[0]==graphicsTextures.views[0].get(),"dynamic candidates missing from sampled hazard list");
  Check(fixture::lastReadStage==int(vk::PipelineStageFlagBits::eAllGraphics),"graphics dynamic candidate read barrier used compute stage");
  const auto preparations=fixture::preparations;
  const std::array feedbackImages{candidates[0]};
  auto feedbackSet=tables.Sync(ctx,graphicsTextures,graphicsSamplers,src,dst,feedbackImages);
  Check(fixture::preparations==preparations,"prepared dynamic draw recollected resources during descriptor finalization");
  Check(fixture::imageLayouts.at({feedbackSet.value,2,0})==int(feedback::Layout),"attachment feedback dynamic descriptor retained GENERAL");
  Check(fixture::imageLayouts.at({feedbackSet.value,3,0})==int(vk::ImageLayout::eGeneral),"unrelated dynamic image captured feedback layout");
  candidates.clear();tables.Prepare(ctx,graphicsTextures,graphicsSamplers,src,dst,candidates);
  Check(tables.Sync(ctx,graphicsTextures,graphicsSamplers,src,dst,feedbackImages)==feedbackSet,"unchanged graphics feedback draw cloned descriptor snapshot");
  auto generalSet=tables.Sync(ctx,graphicsTextures,graphicsSamplers,src,dst);
  Check(generalSet!=feedbackSet,"feedback layout change mutated live graphics set");
  Check(fixture::imageLayouts.at({generalSet.value,2,0})==int(vk::ImageLayout::eGeneral),"next graphics draw failed to restore descriptor GENERAL");
  Check(fixture::imageLayouts.at({feedbackSet.value,2,0})==int(feedback::Layout),"in-flight graphics feedback descriptor changed");
 }
 executor.cycle->completed=true;executor.cycle->Poll(false,true);
 Check(fixture::buffers==0&&fixture::pools.empty(),"graphics snapshot resources leaked after completion");
 std::cout<<"PASS complete production descriptor snapshot lifecycle, copy deltas, sampler holes, resource preparation, faults, graphics stage visibility, feedback layouts and fence lifetime\n";
}
