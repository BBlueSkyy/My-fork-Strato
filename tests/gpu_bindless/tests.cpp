#include <vulkan/vulkan.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
using u16=uint16_t;
using u32=uint32_t;
struct exception : std::runtime_error { template<class... A> exception(const char* s,A&&...) : std::runtime_error(s){} };
struct Backing { vk::ImageLayout layout{vk::ImageLayout::eGeneral}; void PopulateReadBarrier(vk::PipelineStageFlagBits,vk::PipelineStageFlags&,vk::PipelineStageFlags&){} };
struct TextureView { Backing* texture{}; vk::ImageView GetView(){return vk::ImageView(reinterpret_cast<VkImageView>(uintptr_t{15}));} };
struct InterconnectContext { struct { int attached{}; void AttachTexture(TextureView*){++attached;} } executor; };
struct Sampler { vk::Sampler handle{reinterpret_cast<VkSampler>(uintptr_t{42})}; vk::Sampler operator*(){return handle;} };
struct Samplers { int calls{}; bool valid{true}; Sampler sampler; Sampler* GetSampler(InterconnectContext&,u32,u32){++calls;return valid?&sampler:nullptr;} };
namespace Shader { enum class TextureComponentType {Float,Uint,Sint}; }
struct Textures { Shader::TextureComponentType component{Shader::TextureComponentType::Float};
 Shader::TextureComponentType GetTextureComponentType(InterconnectContext&,u32){return component;} bool valid{true}; Backing backing; TextureView view{&backing}; TextureView* GetTexture(InterconnectContext&,u32,int){return valid?&view:nullptr;} };
struct ConstantBuffer { bool view{true}; std::array<u32,32> data{}; template<class T> T Read(auto&,size_t o){return static_cast<T>(data.at(o/4));} };
struct Desc {Shader::TextureComponentType component_type{Shader::TextureComponentType::Float};u32 cbuf_index{},cbuf_offset{},shift_left{},secondary_cbuf_index{1},secondary_cbuf_offset{},secondary_shift_left{20},size_shift{3};bool has_secondary{},is_samplerless{};int type{};u32 count{1},mask{~0U},secondary_mask{~0U};};
#include "production.inc"
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void layouts(){
 struct {size_t totalWriteDescCount{};std::vector<vk::CopyDescriptorSet> copyDescs;std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;} descriptorInfo;
 struct {vk::ShaderStageFlagBits stage{vk::ShaderStageFlagBits::eFragment};} stage;
 u16 bindingIndex{},count{};std::vector<Desc> output;
 #include "layout.inc"
 std::vector<Desc> descs(3);descs[1].is_samplerless=true;descs[1].count=2;descs[2].count=3;
 pushBindings(vk::DescriptorType::eCombinedImageSampler,descs,count,output,[](const auto&,u16){});
 check(descriptorInfo.totalWriteDescCount==3&&count==6,"mixed descriptor types allocate per-binding writes");
 for(size_t i=0;i<3;i++)check(descriptorInfo.descriptorSetLayoutBindings[i].descriptorType==(i==1?vk::DescriptorType::eSampledImage:vk::DescriptorType::eCombinedImageSampler),"graphics layout preserves sampler requirement");
 for(bool individual:{false,true}) {
  std::array<vk::WriteDescriptorSet,8> writes{};std::array<vk::DescriptorImageInfo,8> imageDescs{};u32 writeIdx{},imageIdx{},bindingIdx{};
  #include "writes.inc"
  writeImageDescs(vk::DescriptorType::eCombinedImageSampler,descs,6,[](const auto&,u32){return vk::DescriptorImageInfo{};},individual);
  check(writeIdx==3&&imageIdx==6&&bindingIdx==3,"mixed writes neither spill types nor underallocate");
  for(size_t i=0;i<3;i++)check(writes[i].descriptorType==descriptorInfo.descriptorSetLayoutBindings[i].descriptorType&&writes[i].descriptorCount==descs[i].count,"graphics write/layout agreement");
 }
}
void computeLayouts(){
 struct {size_t totalWriteDescCount{};std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;} descriptorInfo;
 u32 bindingIndex{},count{};
 #include "compute_layout.inc"
 std::vector<Desc> descs(3);descs[1].is_samplerless=true;descs[1].count=2;descs[2].count=3;
 pushBindings(vk::DescriptorType::eCombinedImageSampler,descs,count);
 std::array<vk::WriteDescriptorSet,8>writes{};std::array<vk::DescriptorImageInfo,8>imageDescs{};u32 writeIdx{},imageIdx{},bindingIdx{};
 #include "compute_writes.inc"
 writeImageDescs(vk::DescriptorType::eCombinedImageSampler,descs,[](const auto&,u32){return vk::DescriptorImageInfo{};});
 check(writeIdx==3&&imageIdx==6&&count==6,"compute mixed count agreement");
 for(size_t i=0;i<3;i++)check(writes[i].descriptorType==descriptorInfo.descriptorSetLayoutBindings[i].descriptorType&&writes[i].descriptorType==(i==1?vk::DescriptorType::eSampledImage:vk::DescriptorType::eCombinedImageSampler),"compute write/layout sampler requirement");
}
struct CachedDraw {
 struct DynamicPipeline {bool canRepresent{true};template<class... Args>bool CanBindDynamicDescriptors(Args&...){return canRepresent;}};
 DynamicPipeline *oldPipeline{};
 int ctx{};
 struct {u32 generation{1};u32 GetBindingGeneration(){return generation;}} samplers;
 struct {u32 generation{1};u32 GetBindingGeneration(int){return generation;}} textures;
 struct {bool quickBindEnabled{true};} constantBuffers;
 struct {int invalidations{};void InvalidatePipeline(){++invalidations;}} activeState;
 void*activeDescriptorPipeline{this};u32 activeSamplerBindingGeneration{},activeTextureBindingGeneration{};
 void Refresh(){
 #include "generation.inc"
 }
};
void generationCache(){
 CachedDraw draw;draw.Refresh();check(!draw.activeDescriptorPipeline&&draw.activeState.invalidations==1,"initial pool state invalidates cached descriptor/pipeline");
 draw.activeDescriptorPipeline=&draw;draw.Refresh();check(draw.activeDescriptorPipeline&&draw.activeState.invalidations==1,"unchanged draw must preserve hot caches");
 ++draw.samplers.generation;draw.Refresh();check(!draw.activeDescriptorPipeline&&draw.activeState.invalidations==1,"sampler rebind/mode changes must rebuild descriptors");
 draw.activeDescriptorPipeline=&draw;++draw.textures.generation;draw.Refresh();check(!draw.activeDescriptorPipeline&&draw.activeState.invalidations==2,"TIC pool rebind must rebuild typed pipeline and descriptors");
 draw.activeDescriptorPipeline=&draw;draw.constantBuffers.quickBindEnabled=false;draw.Refresh();check(draw.activeState.invalidations==3,"CBUF changes must reconsider typed specialization");
 draw.constantBuffers.quickBindEnabled=true;CachedDraw::DynamicPipeline dynamic{false};draw.oldPipeline=&dynamic;draw.Refresh();check(draw.activeState.invalidations==4,"dynamic capacity growth must invalidate clean pipeline state");
}
int main(){layouts();computeLayouts();generationCache();
 {InterconnectContext context;Textures typed;std::array<ConstantBuffer,2> buffers{};std::vector<Desc> descriptors(1);
 check(TextureComponentsMatch(context,typed,buffers,descriptors),"matching float pipeline is reusable");
 typed.component=Shader::TextureComponentType::Uint;
 check(!TextureComponentsMatch(context,typed,buffers,descriptors),"uint binding must reject cached float pipeline");
 descriptors[0].component_type=Shader::TextureComponentType::Uint;
 check(TextureComponentsMatch(context,typed,buffers,descriptors),"matching uint pipeline is reusable");
 typed.component=Shader::TextureComponentType::Sint;
 check(!TextureComponentsMatch(context,typed,buffers,descriptors),"signed and unsigned variants remain distinct");
 buffers[0].view=false;
 check(TextureComponentsMatch(context,typed,buffers,descriptors),"unbound texture has no runtime component constraint");
 }

 InterconnectContext ctx; std::array<ConstantBuffer,2> cbuf{}; Desc d;
 cbuf[0].data[0]=7;d.shift_left=20;
 check(ReadBindlessHandle(ctx,cbuf,d,0).samplerIndex==7,"single-source shifted handle must retain shift");
 d.shift_left=0;d.has_secondary=true;cbuf[0].data[0]=19;cbuf[1].data[0]=11;
 check(ReadBindlessHandle(ctx,cbuf,d,0).raw==((11U<<20)|19),"split handles preserve both sources");
 cbuf[0].data[2]=23;cbuf[1].data[2]=5;
 check(ReadBindlessHandle(ctx,cbuf,d,1).raw==((5U<<20)|23),"handle array stride applies to both sources");
 d.has_secondary=false; cbuf[0].view=false;
 check(ReadBindlessHandle(ctx,cbuf,d,0).raw==0,"unbound primary does not read stale data");
 Samplers samplers;Textures textures;vk::PipelineStageFlags src{},dst{};
 d.is_samplerless=true;samplers.valid=false;
 auto b=GetTextureBinding(ctx,d,samplers,textures,BindlessHandle{.raw=(1772U<<20)|19},vk::PipelineStageFlagBits::eFragmentShader,src,dst);
 check(samplers.calls==0,"samplerless fetch must not access TSC pool");check(!b.first.sampler&&b.first.imageView,"sampled-image descriptor retains image without sampler");
 d.is_samplerless=false;bool rejected{};
 try{GetTextureBinding(ctx,d,samplers,textures,BindlessHandle{.raw=(2993U<<20)|19},vk::PipelineStageFlagBits::eFragmentShader,src,dst);}catch(const exception&){rejected=true;}
 check(rejected,"invalid required sampler must never become a Vulkan descriptor");
 samplers.valid=true; auto valid=GetTextureBinding(ctx,d,samplers,textures,BindlessHandle{.raw=19},vk::PipelineStageFlagBits::eFragmentShader,src,dst);
 check(valid.first.sampler==samplers.sampler.handle,"valid sampler object is preserved");
 textures.valid=false; auto nullImage=GetTextureBinding(ctx,d,samplers,textures,BindlessHandle{.raw=19},vk::PipelineStageFlagBits::eFragmentShader,src,dst);
 check(nullImage.first.sampler==samplers.sampler.handle&&!nullImage.first.imageView,"null image retains valid combined descriptor sampler");
 std::cout<<"PASS bindless shifts, split arrays, samplerless fetch, required sampler rejection and null-image descriptor\n";
}
