#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using std::size_t;
#define LOGW(...)
template<class... Args> std::runtime_error exception(const char *message, Args...) { return std::runtime_error(message); }
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> value) : std::span<T>{value} {}
    template<class U> span<U> cast() const { return {reinterpret_cast<U *>(this->data()), this->size_bytes() / sizeof(U)}; }
    span subspan(size_t offset) const { return std::span<T>::subspan(offset); }
    span first(size_t count) const { return std::span<T>::first(count); }
    void copy_from(span source) { std::copy(source.begin(), source.end(), this->begin()); }
};
#include "dirty.inc"
#include "intervals.inc"
#include "tsc.inc"
namespace util {
    template<class T> struct ObjectHash {
        size_t operator()(const T &value) const {
            size_t result{};
            for (auto byte : span<const u8>{reinterpret_cast<const u8 *>(&value), sizeof(value)}) result = result * 31 + byte;
            return result;
        }
    };
}
namespace tsl { template<class K, class V, class H> using robin_map = std::unordered_map<K, V, H>; }
namespace Shader { enum class CompareFunction { Never, Less, Equal, LessThanEqual, Greater, NotEqual, GreaterThanEqual, Always }; }
namespace vk {
    enum class Filter { eNearest, eLinear };
    enum class SamplerMipmapMode { eNearest, eLinear };
    enum class SamplerAddressMode { eRepeat, eMirroredRepeat, eClampToEdge, eClampToBorder, eMirrorClampToEdge };
    enum class CompareOp { eNever, eLess, eEqual, eLessOrEqual, eGreater, eNotEqual, eGreaterOrEqual, eAlways };
    enum class SamplerReductionMode { eWeightedAverage, eMin, eMax };
    enum class BorderColor { eFloatOpaqueWhite, eFloatOpaqueBlack, eFloatTransparentBlack, eFloatCustomEXT };
    enum class Format { eUndefined };
    struct SamplerCreateInfo {
        Filter magFilter, minFilter;
        SamplerMipmapMode mipmapMode;
        SamplerAddressMode addressModeU, addressModeV, addressModeW;
        float mipLodBias;
        bool anisotropyEnable;
        float maxAnisotropy;
        bool compareEnable;
        CompareOp compareOp;
        float minLod, maxLod;
        bool unnormalizedCoordinates;
        BorderColor borderColor{};
    };
    struct SamplerReductionModeCreateInfoEXT { SamplerReductionMode reductionMode; };
    struct SamplerCustomBorderColorCreateInfoEXT { struct { std::array<float, 4> float32; } customBorderColor; Format format; };
    template<class... T> struct StructureChain {
        std::tuple<T...> values;
        StructureChain(T... input) : values{input...} {}
        template<class U> U &get() { return std::get<U>(values); }
        template<class U> void unlink() {}
    };
    namespace raii {
        struct Sampler {
            SamplerCreateInfo info;
            Sampler(int, SamplerCreateInfo value) : info{value} {}
        };
    }
}
namespace skyline::gpu::interconnect {
    namespace engine_common {
        struct TexSamplerPool { u64 offset{}; u32 maximumIndex{}; };
        struct TexHeaderPool { u64 offset{}; u32 maximumIndex{}; };
    }
    struct Registers {
        engine_common::TexSamplerPool sampler;
        engine_common::TexHeaderPool texture;
        u32 padding{};
    };
    using DirtyManager = dirty::Manager<sizeof(Registers), sizeof(u32)>;
    struct Gmmu {
        std::array<std::array<TextureSamplerControl, 4>, 2> pools{};
        auto LookupBlock(u64 offset) {
            auto &pool{pools.at(offset-1)};
            return std::pair{span<std::byte>{reinterpret_cast<std::byte *>(pool.data()), sizeof(pool)}, size_t{0}};
        }
        struct Mapping { u8 *pointer; size_t length; bool valid() const { return pointer; } bool empty() const { return !length; } u8 *data() const { return pointer; } size_t size() const { return length; } };
        auto TranslateRange(u64 offset,size_t bytes) { auto &pool=pools.at(offset-1); return std::vector<Mapping>{{reinterpret_cast<u8*>(pool.data()),std::min(bytes,sizeof(pool))}}; }
    };
    struct InterconnectContext {
        struct Channel { struct AddressSpace { Gmmu gmmu; } *asCtx; } &channelCtx;
        struct { int vkDevice{}; struct {
            bool supportsSamplerMirrorClampToEdge{true}, supportsAnisotropicFiltering{true};
            bool supportsSamplerReductionMode{true}, supportsCustomBorderColor{true};
            struct { bool brokenTextureShadowCompare{}; } quirks;
            struct { float maxSamplerAnisotropy=16,maxSamplerLodBias=16; } dynamicDescriptorLimits;
        } traits; } gpu;
        struct { struct { skyline::IntervalList<u8*> dirtyIntervals,sequencedIntervals; } usageTracker; } executor;
    };
}
#include "samplers_header.inc"
#include "dynamic_pool.inc"
#include "samplers_source.inc"

using namespace skyline::gpu::interconnect;
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }

int main() {
    Registers registers{{1, 3}, {1, 3}};
    DirtyManager manager{registers};
    Samplers samplers{manager, {registers.sampler, registers.texture}};
    InterconnectContext::Channel::AddressSpace addressSpace;
    InterconnectContext::Channel channel{&addressSpace};
    InterconnectContext ctx{channel, {}, {}};
    auto makeSampler{[](TextureSamplerControl::Filter filter) {
        TextureSamplerControl control{};
        control.magFilter = control.minFilter = filter;
        control.mipFilter = TextureSamplerControl::MipFilter::Nearest;
        control.borderColorA = 1;
        return control;
    }};
    for (auto &control : addressSpace.gmmu.pools[0]) control = makeSampler(TextureSamplerControl::Filter::Nearest);
    for (auto &control : addressSpace.gmmu.pools[1]) control = makeSampler(TextureSamplerControl::Filter::Linear);
    samplers.Update(ctx, false);
    const auto firstGeneration{samplers.GetBindingGeneration()};
    samplers.Update(ctx, false);
    Check(samplers.GetBindingGeneration() == firstGeneration, "unchanged sampler pool generation advanced");
    auto *first{samplers.GetSampler(ctx, 1)};
    Check(first->info.minFilter == vk::Filter::eNearest, "initial sampler differs from first pool");
    Check(samplers.GetSampler(ctx, 1) == first, "unchanged pool lost sampler reuse");

    // A register rebind can select an equally sized pool without any cache-invalidate command.
    registers.sampler.offset = 2;
    manager.MarkDirty(offsetof(Registers, sampler) / sizeof(u32));
    samplers.Update(ctx, false);
    Check(samplers.GetBindingGeneration() != firstGeneration, "sampler pool rebind did not invalidate descriptor generation");
    auto *second{samplers.GetSampler(ctx, 1)};
    Check(second->info.minFilter == vk::Filter::eLinear, "same-size sampler pool rebind reused stale index cache");
    Check(first->info.minFilter == vk::Filter::eNearest, "pool rebind destroyed a sampler retained by recorded commands");

    // Programmable linked samplers use the texture index, while explicit methods retain their TSC index.
    addressSpace.gmmu.pools[1][2] = makeSampler(TextureSamplerControl::Filter::Nearest);
    samplers.MarkAllDirty();
    samplers.Update(ctx, true);
    Check(samplers.GetSampler(ctx, 1, 2)->info.minFilter == vk::Filter::eNearest, "linked sampler ignored texture index");
    Check(samplers.GetSampler(ctx, 1)->info.minFilter == vk::Filter::eLinear, "explicit TSC index incorrectly used header binding");
    Check(samplers.GetSampler(ctx, 4) == nullptr, "out-of-range sampler was silently replaced");
    registers.sampler.offset = 1;
    manager.MarkDirty(offsetof(Registers, sampler) / sizeof(u32));
    samplers.Update(ctx, true);
    Check(samplers.GetSampler(ctx, 2, 1) == first, "linked same-size pool rebind reused stale index cache");
    registers.sampler.maximumIndex = 1;
    manager.MarkDirty((offsetof(Registers, sampler) + offsetof(engine_common::TexSamplerPool, maximumIndex)) / sizeof(u32));
    samplers.Update(ctx, true);
    Check(samplers.GetSampler(ctx, 0, 3) != nullptr, "linked pool bounds ignored texture maximum index");
    const auto linkedGeneration{samplers.GetBindingGeneration()};
    samplers.Update(ctx, false);
    Check(samplers.GetBindingGeneration() != linkedGeneration, "ViaHeaderBinding mode did not invalidate descriptor generation");
    Check(samplers.GetSampler(ctx, 3, 0) == nullptr, "independent pool bounds ignored sampler maximum index");
    // Exercise the strict dynamic boundary using actual packed TSC fields and
    // the same production source as the static sampler/cache path above.
    auto valid=makeSampler(TextureSamplerControl::Filter::Nearest);
    valid.floatCoordNormalization=true;
    auto resolve=[&](TextureSamplerControl control) {
        addressSpace.gmmu.pools[0][0]=control;
        samplers.GetPoolHeaders(ctx);
        return samplers.GetDynamicSampler(ctx,0);
    };
    Check(static_cast<bool>(resolve(valid)),"valid normalized dynamic sampler rejected");
    auto invalid=valid;invalid.magFilter=static_cast<TextureSamplerControl::Filter>(0);Check(!resolve(invalid),"reserved filter accepted");
    invalid=valid;invalid.addressModeU=TextureSamplerControl::AddressMode::ClampToBorder;Check(!resolve(invalid),"untyped border accepted for integer images");
    invalid=valid;invalid.addressModeV=TextureSamplerControl::AddressMode::Clamp;Check(!resolve(invalid),"unsupported address mode accepted");
    invalid=valid;invalid.floatCoordNormalization=false;Check(!resolve(invalid),"unnormalized sampler accepted");
    invalid=valid;invalid.depthCompareEnable=true;Check(!resolve(invalid),"comparison sampler accepted");
    invalid=valid;invalid.minLodClamp=2;invalid.maxLodClamp=1;Check(!resolve(invalid),"inverted LOD clamps accepted");
    invalid=valid;invalid.maxAnisotropy=7;ctx.gpu.traits.supportsAnisotropicFiltering=false;Check(!resolve(invalid),"unsupported anisotropy accepted");ctx.gpu.traits.supportsAnisotropicFiltering=true;
    ctx.gpu.traits.dynamicDescriptorLimits.maxSamplerAnisotropy=8;Check(!resolve(invalid),"anisotropy device limit ignored");ctx.gpu.traits.dynamicDescriptorLimits.maxSamplerAnisotropy=16;
    invalid=valid;invalid.mipLodBias=256;ctx.gpu.traits.dynamicDescriptorLimits.maxSamplerLodBias=0.5;Check(!resolve(invalid),"LOD bias device limit ignored");ctx.gpu.traits.dynamicDescriptorLimits.maxSamplerLodBias=16;
    invalid=valid;invalid.reductionFilter=TextureSamplerControl::SamplerReduction::Min;ctx.gpu.traits.supportsSamplerReductionMode=false;Check(!resolve(invalid),"unsupported reduction accepted");ctx.gpu.traits.supportsSamplerReductionMode=true;
    invalid.maxAnisotropy=1;Check(!resolve(invalid),"reduction with anisotropy accepted");
    invalid=valid;invalid.addressModeP=TextureSamplerControl::AddressMode::MirrorClampToEdge;ctx.gpu.traits.supportsSamplerMirrorClampToEdge=false;Check(!resolve(invalid),"unsupported mirror clamp accepted");ctx.gpu.traits.supportsSamplerMirrorClampToEdge=true;
    valid.minFilter=TextureSamplerControl::Filter::Linear;auto retained=resolve(valid);Check(static_cast<bool>(retained)&&samplers.DynamicSamplerUsesLinear(0),"linear dynamic sampler support lost");
    addressSpace.gmmu.pools[0][0].minFilter=TextureSamplerControl::Filter::Nearest;
    Check(samplers.GetSampler(ctx,0)->info.minFilter==vk::Filter::eNearest&&retained->info.minFilter==vk::Filter::eLinear,"dynamic snapshot replaced live static sampler pool");
    Check(!samplers.GetDynamicSampler(ctx,4),"out of range dynamic sampler accepted");
    std::cout << "PASS sampler pool rebinding, cache reuse, retained lifetime, strict dynamic TSC validation, live pool restoration and range checks\n";
}
