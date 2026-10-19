// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.hpp>
#include <boost/container/small_vector.hpp>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <memory>
#include <unordered_map>
#include <span>
#include <stdexcept>
#include <vector>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
#include "shader.inc"
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
template<class... Args> std::runtime_error exception(const char *message, const Args &...) { return std::runtime_error(message); }
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
// The persistence host facade implements byte spans and the checksum boundary.
// The production class, metadata and complete serialisation bodies are unchanged.
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    template<class C> span(C &c) : std::span<T>{c.data(), c.size()} {}
    span(std::span<T> s) : std::span<T>{s} {}
    span subspan(size_t offset, size_t count = std::dynamic_extent) const { return std::span<T>::subspan(offset, count); }
    template<class U> U &as() const { return *reinterpret_cast<U *>(this->data()); }
    template<class U> auto cast() const { return span<U>{reinterpret_cast<U *>(this->data()), this->size_bytes() / sizeof(U)}; }
    template<class C> void copy_from(const C &c) const {
        check(c.size() * sizeof(*c.data()) == this->size_bytes(), "byte span size mismatch");
        std::memcpy(this->data(), c.data(), this->size_bytes());
    }
};
template<class C> span(C &) -> span<typename C::value_type>;
namespace ranges { using std::ranges::find_if; using std::ranges::for_each; }
u64 XXH64(const void *data, size_t size, u64 seed) {
    auto bytes{static_cast<const u8 *>(data)};
    for (size_t i{}; i < size; ++i) seed = (seed ^ bytes[i]) * 1099511628211ULL;
    return seed;
}
namespace skyline::gpu::interconnect { struct ShaderBinary { span<u8> binary; u32 baseOffset; }; }
#include "format.inc"
namespace util { template<class T, class U> constexpr T BitCast(const U &u) { return std::bit_cast<T>(u); } }
#include "tic.inc"
#include "bundle.inc"
namespace skyline::gpu::interconnect {
struct TextureView { struct Texture { bool replaced{}; vk::SampleCountFlagBits sampleCount=vk::SampleCountFlagBits::e1; } *texture; vk::ImageUsageFlags usage=vk::ImageUsageFlagBits::eSampled; vk::ImageUsageFlags GetUsage(){return usage;} };
struct AddressSpace {
    struct Gmmu {
        span<u8> bytes;
        std::pair<span<u8>, size_t> LookupBlock(u64 offset) { return {bytes, offset}; }
        struct Mapping { u8 *pointer; size_t length; bool valid()const{return pointer;} bool empty()const{return !length;} u8 *data()const{return pointer;} size_t size()const{return length;} };
        std::vector<Mapping> dynamicMappings;
        auto TranslateRange(u64,size_t requested) { return dynamicMappings.empty()?std::vector<Mapping>{{bytes.data(),requested}}:dynamicMappings; }
    } gmmu;
};
struct InterconnectContext {
    struct Channel { u64 channelSequenceNumber{}; AddressSpace *asCtx{}; } channelCtx;
    struct { struct { vk::PhysicalDeviceLimits dynamicDescriptorLimits{.maxImageDimension2D=4096}; } traits;
        struct Physical { vk::FormatFeatureFlags features=vk::FormatFeatureFlagBits::eSampledImage|vk::FormatFeatureFlagBits::eSampledImageFilterLinear; unsigned queries{}; vk::FormatProperties getFormatProperties(vk::Format){++queries;return {.optimalTilingFeatures=features};} } vkPhysicalDevice;
    } gpu;
    struct { std::vector<std::shared_ptr<void>> dependencies; void AttachDependency(const std::shared_ptr<void> &resource){dependencies.push_back(resource);} } executor;
};
struct TexturePoolState {
    struct Registers { struct HeaderPool { u64 offset{}; u32 maximumIndex{}; } texHeaderPool; };
    span<TextureImageControl> textureHeaders;
    Registers *engine{};
    u64 bindingGeneration{};
    void Flush(InterconnectContext &ctx);
    unsigned reads{};
    TexturePoolState &UpdateGet(InterconnectContext &) { ++reads; return *this; }
    std::vector<TextureImageControl> dynamicHeaders;
    TexturePoolState &Get(){return *this;}
};
struct Textures {
    TexturePoolState texturePool;
    struct CacheEntry { TextureImageControl tic; TextureView *view; u64 sequenceNumber; u64 bindingGeneration; };
    std::vector<CacheEntry> textureHeaderCache;
    struct RawHash { size_t operator()(const TextureImageControl &tic)const{const auto*p=reinterpret_cast<const u8*>(&tic);size_t hash{};for(size_t i{};i<sizeof(tic);++i)hash=hash*31+p[i];return hash;} };
    struct DynamicMapping { size_t size{}; std::vector<AddressSpace::Gmmu::Mapping> mappings; };
    std::unordered_map<TextureImageControl,DynamicMapping,RawHash> dynamicTextureMappings;
    std::unordered_map<TextureImageControl,std::shared_ptr<TextureView>,RawHash> textureHeaderStore;
    std::unordered_map<vk::Format,vk::FormatFeatureFlags> dynamicFormatFeatures;
    std::shared_ptr<TextureView> nullTextureView;
    TextureView *resolvedView{};
    unsigned dynamicLookups{};
    bool throwLookup{};
    TextureView *GetTexture(InterconnectContext &ctx,u32 index,Shader::TextureType) {
        ++dynamicLookups;
        if(throwLookup) throw std::runtime_error("texture creation failed");
        const auto &textureHeader=texturePool.Get().dynamicHeaders.at(index);
        struct Guest { size_t size; std::vector<AddressSpace::Gmmu::Mapping> mappings; size_t GetSize(){return size;} } guest{16,ctx.channelCtx.asCtx->gmmu.TranslateRange(textureHeader.Iova(),16)};
#include "mapping.inc"
        if(index<textureHeaderCache.size()) textureHeaderCache[index]={textureHeader,resolvedView,ctx.channelCtx.channelSequenceNumber,texturePool.bindingGeneration};
        return resolvedView;
    }
    TextureView *GetDynamicTexture(InterconnectContext&,u32,Shader::TextureType);
    bool DynamicTextureSupportsLinear(InterconnectContext&,u32);
    Shader::TextureComponentType GetDynamicTextureComponentType(u32);
    Shader::TextureComponentType GetTextureComponentType(InterconnectContext &ctx, u32 index);
    u64 GetBindingGeneration(InterconnectContext &ctx);
    TextureView *GetCached(InterconnectContext &ctx, u32 index) {
#include "cache.inc"
        return nullptr;
    }
};
#include "components.inc"
#include "dynamic.inc"
#include "pool.inc"
}
namespace skyline::gpu::interconnect::maxwell3d {
struct RuntimeGraphicsPipelineStateAccessor {
    Textures &textures;
    InterconnectContext &ctx;
    PipelineStateBundle *bundle;
    Shader::TextureComponentType GetTextureComponentType(u32 index) const;
};
struct FilePipelineStateAccessor {
    PipelineStateBundle &bundle;
    Shader::TextureComponentType GetTextureComponentType(u32 index) const;
};
#include "accessors.inc"
}
struct ComponentEnvironment {
    virtual bool IsTexturePixelFormatInteger(u32 handle) = 0;
    virtual Shader::TextureComponentType ReadTextureComponentType(u32 handle) = 0;
};
#include "callbacks.inc"
using namespace skyline::gpu::interconnect;
using Type = Shader::TextureComponentType;
TextureImageControl tic(TextureImageControl::ImageFormat format, TextureImageControl::ImageComponent component) {
    TextureImageControl result{};
    result.formatWord = TextureImageControl::FormatWord{.format = format, .componentR = component, .componentG = component,
        .componentB = component, .componentA = component, .swizzleX = TextureImageControl::ImageSwizzle::R};
    return result;
}
void testDynamicTextureBoundary() {
    using TIC=TextureImageControl;
    auto valid=tic(TIC::ImageFormat::R8,TIC::ImageComponent::Uint);
    valid.addressLow=1;valid.textureType=TIC::TextureType::e2D;valid.headerType=TIC::HeaderType::Pitch;valid.widthMinusOne=7;valid.heightMinusOne=7;
    std::array<u8,32> memory{}; AddressSpace as{{span<u8>{memory},{{memory.data(),8},{memory.data()+8,8}}}};
    InterconnectContext ctx{{0,&as}};
    TextureView::Texture backing;TextureView view{&backing};
    Textures textures;textures.resolvedView=&view;textures.textureHeaderCache.resize(2);
    auto resolve=[&](TIC header){textures.texturePool.dynamicHeaders={header,header};return textures.GetDynamicTexture(ctx,0,Shader::TextureType::Color2D);};
    std::array<TIC,2> live{tic(TIC::ImageFormat::R8,TIC::ImageComponent::Sint),valid};
    textures.texturePool.textureHeaders=span<TIC>{live};
    check(resolve(valid)==&view,"valid dynamic texture rejected");
    check(textures.texturePool.textureHeaders.data()==live.data(),"dynamic lookup replaced live static pool span");
    check(textures.GetDynamicTextureComponentType(0)==Type::Uint&&textures.GetTextureComponentType(ctx,0)==Type::Sint,"dynamic and static numeric queries share stale metadata");
    check(!textures.GetCached(ctx,0),"dynamic lookup left snapshot view in live static fast cache");
    textures.throwLookup=true;try{resolve(valid);}catch(const std::runtime_error&){}textures.throwLookup=false;
    check(textures.texturePool.textureHeaders.data()==live.data(),"failed dynamic lookup did not restore live pool span");
    check(textures.dynamicTextureMappings.at(valid).size==16&&textures.dynamicTextureMappings.at(valid).mappings.size()==2,"full requested guest mappings not recorded");
    check(textures.DynamicTextureSupportsLinear(ctx,0),"linear feature lost");check(ctx.gpu.vkPhysicalDevice.queries==1,"format features not cached");
    auto previous=std::make_shared<TextureView>(TextureView{&backing});std::weak_ptr<TextureView> retained=previous;
    textures.textureHeaderStore[valid]=previous;
    textures.textureHeaderCache[1]={valid,previous.get(),0,0};previous.reset();
    check(resolve(valid)==&view&&textures.textureHeaderStore.contains(valid),"unchanged mapping erased texture store");
    as.gmmu.dynamicMappings[1].pointer=memory.data()+16;
    check(resolve(valid)==&view&&!textures.textureHeaderStore.contains(valid),"tail mapping remap reused old texture view");
    check(!textures.textureHeaderCache[1].view,"same-TIC alias retained dangling view after remap");
    check(!retained.expired(),"remap released image view used by queued commands");ctx.executor.dependencies.clear();check(retained.expired(),"retired remap view leaked");
    as.gmmu.dynamicMappings[1].pointer=nullptr;check(!resolve(valid),"unmapped dynamic image tail accepted");as.gmmu.dynamicMappings[1].pointer=memory.data()+8;
    auto invalid=valid;invalid.viewConfig.msaaMode=static_cast<TIC::MsaaMode>(1);check(!resolve(invalid),"multisample TIC accepted");
    invalid=valid;invalid.isSparse=true;check(!resolve(invalid),"sparse TIC accepted");
    invalid=valid;invalid.depthMinusOne=1;check(!resolve(invalid),"array/depth TIC accepted");
    invalid=valid;invalid.viewLayerBase_0_2=1;check(!resolve(invalid),"nonzero base layer accepted");
    invalid=valid;invalid.widthMinusOne=4096;check(!resolve(invalid),"device image dimension limit ignored");
    invalid=valid;invalid.mipMaxLevels=4;check(!resolve(invalid),"mip count exceeds extent");
    invalid=valid;invalid.viewConfig.mipMinLevel=2;invalid.viewConfig.mipMaxLevel=1;check(!resolve(invalid),"inverted mip view accepted");
    invalid=valid;invalid.viewConfig.mipMaxLevel=1;check(!resolve(invalid),"mip view exceeds backing count");
    invalid=valid;invalid.formatWord.swizzleX=static_cast<TIC::ImageSwizzle>(1);check(!resolve(invalid),"constant-one swizzle accepted");
    textures.dynamicFormatFeatures.clear();ctx.gpu.vkPhysicalDevice.features={};check(!resolve(valid),"unsupported sampled format accepted");
    textures.dynamicFormatFeatures.clear();ctx.gpu.vkPhysicalDevice.features=vk::FormatFeatureFlagBits::eSampledImage;
    check(resolve(valid)==&view&&!textures.DynamicTextureSupportsLinear(ctx,0),"nonlinear sampled format rejected or linear feature invented");
    view.usage={};check(!resolve(valid),"texture missing sampled usage accepted");view.usage=vk::ImageUsageFlagBits::eSampled;
    backing.sampleCount=vk::SampleCountFlagBits::e2;check(!resolve(valid),"multisampled backing accepted");
}
int main(int argc, char **argv) {
    try {
        using F = TextureImageControl::ImageFormat;
        using C = TextureImageControl::ImageComponent;
        using S = TextureImageControl::ImageSwizzle;
        std::vector<TextureImageControl> headers;
        for (auto f : {F::R8, F::R16, F::R8G8, F::R32, F::R16G16, F::A8B8G8R8, F::A2B10G10R10, F::R32G32, F::R16G16B16A16, F::R32G32B32A32}) {
            headers.push_back(tic(f, C::Uint));
            headers.push_back(tic(f, C::Sint));
        }
        auto normalized{tic(F::A8B8G8R8, C::Unorm)};
        normalized.isSrgb = true;
        headers.push_back(normalized);
        headers.push_back(tic(F::R16G16B16A16, C::Float));
        auto stencil{tic(F::S8D24, C::Unorm)};
        stencil.formatWord.componentR = C::Uint;
        headers.push_back(stencil);
        auto depth{stencil}; depth.formatWord.swizzleX = S::G;
        headers.push_back(depth);
        auto depthFirst{tic(F::D24S8, C::Uint)};
        depthFirst.formatWord.componentR = C::Unorm;
        headers.push_back(depthFirst);
        depthFirst.formatWord.swizzleX = S::G;
        headers.push_back(depthFirst);
        headers.push_back({});
        Textures textures{.texturePool = {.textureHeaders=headers,.bindingGeneration=1}};
        AddressSpace as{{span<u8>{reinterpret_cast<u8 *>(headers.data()), headers.size() * sizeof(TextureImageControl)}}};
        InterconnectContext ctx{{0, &as}};
        TexturePoolState::Registers registers{{0, 0}};
        TexturePoolState pool{.engine=&registers};
        pool.Flush(ctx);
        check(pool.bindingGeneration == 1 && pool.textureHeaders.data() == headers.data(), "initial pool flush did not expose generation");
        registers.texHeaderPool.offset = sizeof(TextureImageControl);
        pool.Flush(ctx);
        check(pool.bindingGeneration == 2 && pool.textureHeaders.data() == headers.data() + 1, "same-size pool rebind did not advance generation");
        registers.texHeaderPool.maximumIndex = 1;
        pool.Flush(ctx);
        check(pool.bindingGeneration == 3 && pool.textureHeaders.size() == 2, "pool limit refresh did not advance generation");
        for (int i{}; i < 100; ++i)
            check(textures.GetBindingGeneration(ctx) == 1, "stable pool generation changed on reads");
        for (u32 i{}; i < 20; ++i)
            check(textures.GetTextureComponentType(ctx, i) == (i % 2 ? Type::Sint : Type::Uint), "integer TIC family lost numeric signedness");
        for (u32 i : {20U, 21U, 23U, 24U, 26U, 99U})
            check(textures.GetTextureComponentType(ctx, i) == Type::Float, "float, depth or invalid TIC classification changed");
        for (u32 i : {22U, 25U})
            check(textures.GetTextureComponentType(ctx, i) == Type::Uint, "stencil view must use unsigned numeric type");
        TextureView::Texture backing;
        TextureView view{&backing};
        textures.textureHeaderCache.resize(headers.size());
        textures.textureHeaderCache[0] = {headers[0], &view, 0, 1};
        check(textures.GetCached(ctx, 0) == &view, "unchanged pool cache missed");
        auto rebound{headers}; rebound[0] = headers[1];
        textures.texturePool.textureHeaders = rebound;
        ++textures.texturePool.bindingGeneration;
        check(textures.GetCached(ctx, 0) == nullptr, "same-sequence pool rebind reused stale view");
        textures.texturePool.textureHeaders = headers;
        check(textures.GetCached(ctx, 0) == &view && textures.textureHeaderCache[0].bindingGeneration == 2, "equal TIC did not refresh pool generation");
        for (auto type : {Type::Float, Type::Uint, Type::Sint}) {
            GraphicsEnvironment graphics;
            ComputeEnvironment compute;
            graphics.getTextureComponentType = [type](u32 handle) { check(handle == 17, "graphics callback lost TIC handle"); return type; };
            compute.getTextureComponentType = [type](u32 handle) { check(handle == 17, "compute callback lost TIC handle"); return type; };
            check(graphics.ReadTextureComponentType(17) == type && compute.ReadTextureComponentType(17) == type, "shader environment lost component type");
            check(graphics.IsTexturePixelFormatInteger(17) == (type != Type::Float) && compute.IsTexturePixelFormatInteger(17) == (type != Type::Float), "legacy integer query disagrees with numeric query");
        }
        PipelineStateBundle captured;
        skyline::gpu::interconnect::maxwell3d::RuntimeGraphicsPipelineStateAccessor accessor{textures, ctx, &captured};
        check(accessor.GetTextureComponentType(1) == Type::Sint, "runtime accessor lost signed type");
        skyline::gpu::interconnect::maxwell3d::FilePipelineStateAccessor fileAccessor{captured};
        check(fileAccessor.GetTextureComponentType(1) == Type::Sint, "file accessor lost signed type");
        PipelineStateBundle bundle;
        const u32 key{42};
        bundle.Reset(key);
        for (u32 i{}; i < 3; ++i) bundle.AddTextureComponentType(i, static_cast<Type>(i));
        const char *path{argc > 1 ? argv[1] : "/tmp/strato-component-bundle.bin"};
        { std::ofstream out(path, std::ios::binary); bundle.Serialise(out); }
        PipelineStateBundle restored;
        { std::ifstream in(path, std::ios::binary); check(restored.Deserialise(in), "bundle not restored"); check(!restored.Deserialise(in), "bundle EOF not recognized"); }
        std::remove(path);
        check(restored.GetKey<u32>() == key, "bundle key changed");
        for (u32 i{}; i < 3; ++i)
            check(restored.LookupTextureComponentType(i) == static_cast<Type>(i), "bundle lost numeric component type");
        bundle.Reset(key);
        bool missing{};
        try { bundle.LookupTextureComponentType(1); } catch (const std::runtime_error &) { missing = true; }
        check(missing, "reset retained numeric component type");
        testDynamicTextureBoundary();
        std::cout << "PASS production TIC signedness, stencil aspects, pool cache refresh, strict dynamic textures/remaps and numeric bundle round-trip\n";
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
