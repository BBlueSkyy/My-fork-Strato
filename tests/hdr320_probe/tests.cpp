// SPDX-License-Identifier: MPL-2.0
#include <array>
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
#include <cstdio>
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.hpp>
#include <fmt/format.h>
#include <texture/hdr_route_probe.h>
using u32 = uint32_t;
using u64 = uint64_t;
using nullptr_t = std::nullptr_t;
struct exception : std::runtime_error {
    template<class... T> exception(const char *,T&&...) : std::runtime_error("unused host span error") {}
};
namespace util { constexpr bool IsAligned(size_t n,size_t a){return n%a==0;} }
namespace debug { inline std::string CaptureBacktrace(){return {};} }
#include "span.inc"
namespace diagnostic = skyline::gpu::diagnostic;
std::vector<std::string> logs;
#define LOGI(...) logs.push_back(fmt::format(__VA_ARGS__))
struct TextureImageControl { std::array<u32,8> words{}; };
struct TextureSamplerControl {
    u32 minFilter{},magFilter{},mipFilter{};
    std::array<u32,5> extra{};
    float MinLodClamp(){return 0;}
    float MaxLodClamp(){return 8;}
    float MipLodBias(){return -1;}
};
static_assert(sizeof(TextureSamplerControl)==32);
struct Texture {
    struct {u32 width{640},height{360};} dimensions;
    struct Format {vk::Format vkFormat{vk::Format::eB10G11R11UfloatPack32};} formatValue;
    Format *format{&formatValue};
    u32 levelCount{2};
  private:
    u64 backingGeneration{1};
  public:
#include "getter.inc"
    void AdvanceBackingGeneration(){++backingGeneration;}
    vk::Image image{std::bit_cast<VkImage>(u64{123})};
    vk::Image GetBacking(){return image;}
};
struct TextureView {std::shared_ptr<Texture> texture;vk::ImageSubresourceRange range;};
struct Textures {
    struct Entry {TextureImageControl tic;TextureView *view;};
    std::vector<Entry> textureHeaderCache;
    bool ObserveBoundTic(u32,const TextureView*,std::array<u32,8>&)const;
};
struct FakeSampler {vk::Sampler native;vk::Sampler operator*()const{return native;}};
struct Samplers {
    struct Pool {bool didUseTexHeaderBinding{};Pool &Get(){return *this;}} samplerPool;
    struct Entry {std::unique_ptr<FakeSampler> sampler;vk::SamplerCreateInfo observedInfo;};
    std::vector<std::pair<TextureSamplerControl,Entry>> texSamplerStore;
    u32 ObserveBoundSamplerIndex(u32,u32);
    bool ObserveBoundSampler(u64,TextureSamplerControl&,vk::SamplerCreateInfo&)const;
};
struct InterconnectContext {struct{u64 channelSequenceNumber{5};}channelCtx;};
union BindlessHandle {u32 raw;struct{u32 textureIndex:20;u32 samplerIndex:12;};};
#include "observation.inc"
struct Pipeline {struct{std::array<u64,6>shaderHashes{};}sourcePackedState;};
struct State {Pipeline pipeline;std::array<TextureView*,1>targets{};
    Pipeline *GetPipeline(){return &pipeline;}
    auto GetColorAttachments(){return targets;}
};
struct Draw {
    State activeState;InterconnectContext ctx;
    std::vector<TextureView*> activeDescriptorSetSampledImages;
    std::array<diagnostic::HdrSampleBinding,8> hdrBindings{};
    u32 hdrObservedDraws{};std::array<u64,diagnostic::HdrDrawLimit> hdrObservedSignatures{};Samplers samplers;vk::Rect2D scissor{{0,0},{320,180}};
    void Record(){
#include "draw.inc"
    }
};
void CheckSpanCap(u32 hdrObservedDraws) {
    std::array<diagnostic::HdrSampleBinding,8> hdrBindings{};
#include "span_cap.inc"
    assert(observedBindings.size() == (hdrObservedDraws < diagnostic::HdrDrawLimit ? 8 : 0));
}
int main(){
    CheckSpanCap(0);CheckSpanCap(12);
    assert(diagnostic::HdrDrawSelected(diagnostic::HdrReductionShader,320,180,0));
    assert(!diagnostic::HdrDrawSelected(diagnostic::HdrReductionShader,640,360,0));
    assert(!diagnostic::HdrDrawSelected(1,320,180,0));
    assert(!diagnostic::HdrDrawSelected(diagnostic::HdrReductionShader,320,180,12));
    Draw d;Textures textures;auto image=std::make_shared<Texture>();
    TextureView sample{image,{vk::ImageAspectFlagBits::eColor,0,2,0,1}};
    TextureView target{image,{vk::ImageAspectFlagBits::eColor,1,1,0,1}};
    textures.textureHeaderCache.push_back({{{1,2,3,4,5,6,7,8}},&sample});
    vk::Sampler native{std::bit_cast<VkSampler>(u64{99})};
    Samplers::Entry entry{std::make_unique<FakeSampler>(FakeSampler{native}),
        vk::SamplerCreateInfo{.magFilter=vk::Filter::eLinear,.minFilter=vk::Filter::eLinear,
        .mipmapMode=vk::SamplerMipmapMode::eNearest,.mipLodBias=-1,.minLod=0,.maxLod=.25f}};
    d.samplers.texSamplerStore.emplace_back(TextureSamplerControl{},std::move(entry));
    struct Desc {u32 cbuf_index{2},cbuf_offset{32};}desc;
    std::pair<vk::DescriptorImageInfo,TextureView*> binding{{native,std::bit_cast<VkImageView>(u64{100}),vk::ImageLayout::eGeneral},&sample};
    ObserveHdrBinding(d.hdrBindings,0,d.ctx,textures,d.samplers,BindlessHandle{3u<<20},desc,0,vk::PipelineStageFlagBits::eFragmentShader,binding);
    auto saved=d.hdrBindings[0];
    assert(saved.ticKnown && saved.tic[7]==8 && saved.baseMip==0 && saved.mipCount==2);
    assert(saved.effectiveTscIndex==3 && saved.image==123 && saved.sampler==99);
    d.samplers.samplerPool.didUseTexHeaderBinding=true;
    assert(d.samplers.ObserveBoundSamplerIndex(3,0)==0);
    d.activeState.pipeline.sourcePackedState.shaderHashes[5]=diagnostic::HdrReductionShader;
    d.activeState.targets[0]=&target;d.activeDescriptorSetSampledImages.push_back(&sample);
    d.Record();
    assert(d.hdrObservedDraws==1 && logs.size()==4);
    assert(logs[0].find("HDR320_DRAW")!=std::string::npos);
    assert(logs[2].find("effective_lod=0..0.25")!=std::string::npos);
    assert(logs[3].find("mip=1+1")!=std::string::npos);
    // Guest pool changes cannot rewrite the historical descriptor snapshot.
    textures.textureHeaderCache[0].tic.words.fill(42);
    image->AdvanceBackingGeneration();image->image=std::bit_cast<VkImage>(u64{456});
    ++d.ctx.channelCtx.channelSequenceNumber;d.Record();
    assert(d.hdrBindings[0].tic[7]==8 && d.hdrBindings[0].image==123);
    // Partial update replaces only its own slot; unchanged slots survive.
    ObserveHdrBinding(d.hdrBindings,1,d.ctx,textures,d.samplers,BindlessHandle{0},desc,0,vk::PipelineStageFlagBits::eFragmentShader,binding);
    assert(d.hdrBindings[0].tic[7]==8 && d.hdrBindings[1].tic[7]==42);
    auto oldCount=d.hdrObservedDraws;d.Record();assert(d.hdrObservedDraws==oldCount);
    for(int i=0;i<20;++i){image->AdvanceBackingGeneration();d.Record();}
    assert(d.hdrObservedDraws==12 && logs.size()==48);
    TextureSamplerControl missingTsc{};vk::SamplerCreateInfo missingInfo{};
    assert(!d.samplers.ObserveBoundSampler(999,missingTsc,missingInfo));
    std::puts("PASS: production binding snapshots, effective sampler metadata, quick reuse/partial slots, native identity, filters, draw gate/cap and log formatting; no GPU operations");
}
