// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <source_location>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef TIC_VULKAN_LIFETIME
#include "vulkan_lifetime.h"
#endif
using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
namespace Shader {
#include "shader-type.inc"
}
namespace skyline::util { template<class T, class U> constexpr T BitCast(U value) { return std::bit_cast<T>(value); } }
#include <gpu/interconnect/common/tic.h>
#include <gpu/interconnect/common/null_texture_policy.h>
#include <gpu/interconnect/common/tic_validation_policy.h>
#include <gpu/interconnect/common/pipeline_specialization.h>
#define LOGW(...) ((void)0)
template<class... T> std::runtime_error exception(const char *s, T...) { return std::runtime_error(s); }
constexpr u64 VK_NULL_HANDLE = 0;
namespace vk {
using PipelineStageFlags = u32; using Sampler = u64;
enum class PipelineStageFlagBits { eFragmentShader, eComputeShader };
enum class ImageLayout { eShaderReadOnlyOptimal, eGeneral };
struct DescriptorImageInfo { Sampler sampler{}; u64 imageView{}; ImageLayout imageLayout{}; };
struct ImageUsageFlagBits { static constexpr u32 eStorage = 1; };
}
namespace skyline {
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    explicit span(T &object) : std::span<T>(&object, 1) {}
    template<class U> span<U> cast() const { return span<U>(reinterpret_cast<U*>(this->data()), this->size_bytes()/sizeof(U)); }
    bool valid() const { return this->data() != nullptr; }
};
namespace soc::gm20b {
#include "gmmu-bits.inc"
}
// Run the exact production LookupBlock methods. Only the segment-table storage
// is a map facade; supplied entries carry the same base/extent/sparse fields.
struct Gmmu {
    using VaType = u64;
    struct Entry { u64 virt{}; u8 *phys{}; u64 extent{}; struct { bool sparseMapped{}; } extraInfo; };
    struct Table {
        std::unordered_map<u64, Entry> entries;
        Entry empty{};
        const Entry &operator[](u64 address) const {
            for (auto &[base, entry] : entries)
                if (address >= base && address - base < entry.extent) return entry;
            return empty;
        }
    } blockSegmentTable;
    std::shared_mutex blockMutex;
#include "lookup.inc"
};
namespace gpu::interconnect {
struct Resource {
    bool replaced{}; vk::ImageLayout layout{vk::ImageLayout::eShaderReadOnlyOptimal};
    int writes{};
    void PopulateReadBarrier(vk::PipelineStageFlagBits, u32&, u32&) {}
    void TransitionLayout(vk::ImageLayout value) { layout = value; }
    void MarkGpuDirty(int) { ++writes; }
};
struct TextureView {
    std::shared_ptr<Resource> texture{std::make_shared<Resource>()};
    u64 GetView(bool = false) { return 42; }
    u32 GetUsage() { return vk::ImageUsageFlagBits::eStorage; }
};
struct InterconnectContext {
    struct AddressSpace { Gmmu gmmu; } as;
    struct Channel { AddressSpace *asCtx; u64 channelSequenceNumber{1}; } channelCtx{&as};
    struct Gpu { struct { bool supportsNullDescriptor{true}; } traits; } gpu;
    struct Executor { int usageTracker{}; int attachments{}; void AttachTexture(TextureView*) { ++attachments; } } executor;
};
struct Pool {
    std::vector<TextureImageControl> data{1};
    struct State { std::span<TextureImageControl> textureHeaders; };
    State UpdateGet(InterconnectContext&) { return {data}; }
};
struct Textures {
    struct CacheEntry { TextureImageControl tic{}; Shader::TextureType shaderType{}; TextureView *view{}; u64 sequenceNumber{}; };
    Pool texturePool;
    std::vector<CacheEntry> textureHeaderCache;
    std::shared_ptr<TextureView> nullTextureView;
    TextureView validView;
    TextureView *GetTexture(InterconnectContext&, u32, Shader::TextureType, NullTextureReason*);
    Shader::TextureType GetTextureType(InterconnectContext&, u32, bool diagnostics = true);
    u32 integerFormat{};
    bool IsTexturePixelFormatInteger(InterconnectContext&, u32, bool = true) { return integerFormat; }
};
static std::shared_ptr<TextureView> CreateNullTexture(InterconnectContext&) { return std::make_shared<TextureView>(); }
#include "textures.inc"
struct Samplers {
    struct Sampler { u64 handle{17}; u64 operator*() const { return handle; } } sampler, nullSampler{23};
    u32 samplerPoolSize{4096};
    u32 comparison{};
    std::unordered_map<u32,u32> bySampler;
    u32 GetTextureCompareFunc(InterconnectContext&, u32 samplerIndex, u32, bool = true) { auto i=bySampler.find(samplerIndex); return i==bySampler.end()?comparison:i->second; }
    Sampler *GetNullSampler(InterconnectContext&) { return &nullSampler; }
    Sampler *GetSampler(InterconnectContext&, u32 index, u32) { return index < samplerPoolSize ? &sampler : nullptr; }
};
#include "bindings.inc"
// Real cache-hit/transition functions; only pipeline construction is a facade.
// Capture precisely the descriptor metadata available when compilation occurs.
struct PackedPipelineState { int key{}; int shaderHashes{}; bool operator==(const PackedPipelineState&) const = default; };
struct KeyHash { size_t operator()(const PackedPipelineState &v) const { return v.key; } };
using CommandExecutor = InterconnectContext::Executor;
struct ConstantBufferSet {
    struct WordBuffer {
        std::function<void()> beforeRead;
        std::array<u32, 64> words{};
        struct View { bool bound{true}; size_t size{256}; explicit operator bool() const { return bound; } } view;
        int reads{};
        void Read(CommandExecutor&, span<u8> bytes, size_t offset, std::source_location, bool retry) {
            ++reads;
            if (!view.bound || offset > view.size || bytes.size() > view.size-offset)
                throw std::runtime_error("fixture detected unsafe buffer access");
            if (!retry) throw std::runtime_error("specialization must use retryable read");
            if (beforeRead) { auto callback=std::move(beforeRead); beforeRead={}; callback(); }
            std::memcpy(bytes.data(), reinterpret_cast<u8*>(words.data()) + offset, bytes.size());
        }
#include "constant-read.inc"
    };
    struct Slot {
        std::array<WordBuffer, 32> buffers{};
        WordBuffer &operator[](size_t n) { return buffers.at(n); }
        template<class T> T ReadForShaderCompile(InterconnectContext::Executor &e, u32 offset) { return buffers[0].ReadForShaderCompile<T>(e, offset); }
    };
    std::array<Slot, 5> stages{};
    Slot &operator[](size_t n) { return stages.at(n); }
};
struct ShaderBinary {};
namespace engine { constexpr size_t PipelineCount = 1; }
struct PipelineStateBundle { void Reset(const PackedPipelineState&) {} };
struct RuntimeGraphicsPipelineStateAccessor {
    InterconnectContext &ctx; Textures &textures; Samplers &samplers; ConstantBufferSet &cbufs;
    u32 GetConstantBufferValue(u32 stage,u32 index,u32 offset) { return cbufs[stage][index].ReadForShaderCompile<u32>(ctx.executor,offset); }
    Shader::TextureType GetTextureType(u32 index) { return textures.GetTextureType(ctx,index); }
    bool IsTexturePixelFormatInteger(u32 index) { return textures.IsTexturePixelFormatInteger(ctx,index); }
    u32 GetTextureCompareFunc(u32 raw) { BindlessHandle h{.raw=raw}; return samplers.GetTextureCompareFunc(ctx,h.samplerIndex,h.textureIndex); }
    RuntimeGraphicsPipelineStateAccessor(std::unique_ptr<PipelineStateBundle>, InterconnectContext &c, Textures &t, Samplers &s, ConstantBufferSet &b, const std::array<ShaderBinary, 1>&) : ctx(c), textures(t), samplers(s), cbufs(b) {}
};
struct DescriptorUpdateInfo { int kind; };
inline DescriptorUpdateInfo fullUpdate{1}, quickUpdate{2};
inline bool testScalarFold{}, testSplitHandle{};
struct Pipeline {
    PackedPipelineState sourcePackedState;
    PipelineSpecialization specialization;
    Shader::TextureType metadata;
    std::array<Pipeline*, 2> transitionCache{};
    static inline int constructions{}, destructions{};
#ifdef TIC_VULKAN_LIFETIME
    std::unique_ptr<PipelineLifetimeToken> lifetime;
#endif
    int stageMask{}, descriptorInfo{};
    struct { bool usesPushDescriptors{}; } compiledPipeline;
    std::unordered_map<Pipeline*, bool> bindingMatchCache;
    bool CheckBindingMatch(Pipeline*);
    void InheritDescriptorBufferViews(const Pipeline&) {}
    DescriptorUpdateInfo *SyncDescriptors(InterconnectContext&, ConstantBufferSet&, Samplers&, Textures&, std::vector<TextureView*>&, u32&, u32&) { return &fullUpdate; }
    DescriptorUpdateInfo *SyncDescriptorsQuickBind(InterconnectContext&, ConstantBufferSet&, Samplers&, Textures&, int, std::vector<TextureView*>&, u32&, u32&) { return &quickUpdate; }
    Pipeline(InterconnectContext::Gpu&, RuntimeGraphicsPipelineStateAccessor &a, const PackedPipelineState &s) : sourcePackedState(s) {
        ++constructions;
#ifdef TIC_VULKAN_LIFETIME
        lifetime = PipelineLifetimeRuntime::active->Create();
#endif
        auto &accessor = a;
        constexpr u32 i = 0;
        auto query =
#include "graphics-query.inc"
        ;
        Shader::TextureHandleSource source{0,0,0,testSplitHandle?1u:0u,testSplitHandle?4u:0u,testSplitHandle?20u:0u};
        metadata = static_cast<Shader::TextureType>(query(source,Shader::TextureQueryKind::Type));
        query(source,Shader::TextureQueryKind::Integer);
        query(source,Shader::TextureQueryKind::Compare);
        if (testScalarFold) {
            auto scalar =
#include "scalar-read.inc"
            ;
            scalar(1,4);
        }
    }
    ~Pipeline() { ++destructions; }
    bool MatchesSpecialization(InterconnectContext&, Textures&, Samplers&, ConstantBufferSet&) const;
    Pipeline *LookupNext(const PackedPipelineState&, InterconnectContext&, Textures&, Samplers&, ConstantBufferSet&);
};
struct PipelineManager {
    std::unordered_map<PackedPipelineState, std::vector<std::unique_ptr<Pipeline>>, KeyHash> map;
    Pipeline *FindOrCreate(InterconnectContext&, Textures&, Samplers&, ConstantBufferSet&, const PackedPipelineState&, const std::array<ShaderBinary, 1>&);
};
#include "cache.inc"
namespace compute {
struct Pipeline {
    PipelineSpecialization specialization;
    Shader::TextureType metadata;
    Pipeline(InterconnectContext &c, Textures &t, Samplers &samplers, ConstantBufferSet &b, const PackedPipelineState&, const ShaderBinary&) {
        using K = PipelineSpecialization::Kind;
        u32 handle = b[0].ReadForShaderCompile<u32>(c.executor, 0);
        PipelineSpecialization::TextureSource source{0,0,0,0,0,0};
        metadata = t.GetTextureType(c, handle & 0xFFFFF);
        specialization.RecordTexture(K::TextureType, 0, source, static_cast<u32>(metadata));
        specialization.RecordTexture(K::TextureInteger, 0, source, t.integerFormat);
        specialization.RecordTexture(K::SamplerCompare, 0, source, samplers.comparison);
    }
    bool MatchesSpecialization(InterconnectContext&, Textures&, Samplers&, ConstantBufferSet&) const;
};
#include "compute-match.inc"

struct Manager {
    std::unordered_map<PackedPipelineState, std::vector<std::unique_ptr<Pipeline>>, KeyHash> map;
#include "compute-cache.inc"
};
}
namespace dirty { struct Handle { bool *value; }; }
#include "dirty.inc"
struct StateUpdateBuilder {};
struct PipelineState : dirty::CachedManualDirty, dirty::RefreshableManualDirty {
    PipelineManager manager;
    Pipeline *pipeline{};
    int flushes{};
    explicit PipelineState(dirty::Handle) {}
    void Flush(InterconnectContext &ctx, Textures &t, Samplers &s, ConstantBufferSet &b, StateUpdateBuilder&) {
        ++flushes;
        pipeline = manager.FindOrCreate(ctx, t, s, b, {}, {});
    }
    bool Refresh(InterconnectContext&, Textures&, Samplers&, ConstantBufferSet&, StateUpdateBuilder&);
    void PurgeCaches() { pipeline = nullptr; }
};
#include "refresh.inc"
}
}
using namespace skyline;
using namespace skyline::gpu::interconnect;
using T = Shader::TextureType;
static DescriptorUpdateInfo *PrepareDescriptors(InterconnectContext &ctx, Textures &textures, Samplers &samplers, ConstantBufferSet &bound,
                                               Pipeline *activeDescriptorPipeline, Pipeline *pipeline, bool quick,
                                               u64 cachedEpoch = 0, u64 currentEpoch = 0) {
    // The extracted production lambda captures these Maxwell3D state values.
    // The host fixture must model both; otherwise the actual epoch guard
    // cannot compile or be checked when the same pipeline is quick-bound.
    u64 activeDescriptorViewEpoch{cachedEpoch};
    const u64 viewEpoch{currentEpoch};
    struct { ConstantBufferSet &boundConstantBuffers; bool quickBindEnabled{true}; std::optional<int> quickBind; } constantBuffers{bound, true, quick ? std::optional<int>{0} : std::nullopt};
    std::vector<TextureView*> activeDescriptorSetSampledImages;
    u32 srcStageMask{},dstStageMask{};
    auto prepare =
#include "prepare-descriptors.inc"
    ;
    return prepare();
}
using Tic = TextureImageControl;
static int checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (false)
template<class F> bool Throws(F f) { try { f(); return false; } catch (const std::runtime_error&) { return true; } }
static void SetTic(Textures &textures, u32 type, u64 address) {
    Tic tic{};
    tic.textureType = static_cast<Tic::TextureType>(type);
    tic.addressLow = address;
    tic.addressHigh = address >> 32;
    textures.texturePool.data[0] = tic;
}
struct Desc { T type; bool is_written{true}; };
int main(int argc, char **argv) {
#ifndef TIC_VULKAN_LIFETIME
    (void)argv;
#else
    if (argc < 3) return 2;
    PipelineLifetimeRuntime runtime{argv[2]};
#endif
    InterconnectContext ctx;
    Textures textures;
    Samplers samplers;
    std::array<u8, 4096> backing{};
    ctx.as.gmmu.blockSegmentTable.entries[0x1000] = {0x1000, backing.data(), backing.size(), {false}};
    u32 src{}, dst{};
    if (argc > 1) {
        SetTic(textures, 15, 0x8000);
        PipelineManager manager; compute::Manager computeManager;
        ConstantBufferSet cbufs; PackedPipelineState key; std::array<ShaderBinary, 1> binaries{};
        auto first = manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries);
        auto firstCompute = computeManager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries[0]);
        CHECK(first->metadata == T::Color2D);
        CHECK(firstCompute->metadata == T::Color2D);
#ifdef TIC_VULKAN_LIFETIME
        runtime.SubmitBlocked(*first->lifetime);
#endif
        SetTic(textures, 6, 0x1000);
        CHECK(textures.GetTextureType(ctx, 0) == T::Buffer);
        auto reused = manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries);
        auto reusedCompute = computeManager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries[0]);
        // Original failure expectations: stale variants must be rejected and the
        // managers must select a new correctly specialized variant.
        CHECK(reused->metadata == T::Buffer);
        CHECK(first->LookupNext(key, ctx, textures, samplers, cbufs) == nullptr);
        auto otherKey = key; otherKey.key = 99;
        RuntimeGraphicsPipelineStateAccessor accessor{std::make_unique<PipelineStateBundle>(), ctx, textures, samplers, cbufs, binaries};
        Pipeline other{ctx.gpu, accessor, otherKey};
        other.transitionCache[0] = first;
        CHECK(other.LookupNext(key, ctx, textures, samplers, cbufs) == nullptr);
        CHECK(reusedCompute->metadata == T::Buffer);
        CHECK(first != reused && firstCompute != reusedCompute);
        CHECK(first->metadata == T::Color2D); // old pending raw reference stays live
        const int compiled = Pipeline::constructions;
        CHECK(manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries) == reused);
        CHECK(Pipeline::constructions == compiled);
        SetTic(textures, 15, 0x8000); // restore original invalid state: reuse, no compile
        CHECK(manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries) == first);
        CHECK(computeManager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries[0]) == firstCompute);
        CHECK(Pipeline::constructions == compiled);
        CHECK(first->LookupNext(key, ctx, textures, samplers, cbufs) == first);
        CHECK(other.LookupNext(key, ctx, textures, samplers, cbufs) == first);
        CHECK(manager.map.at(key).size() == 2);
        // An unrelated TIC must not recompile this shader.
        textures.texturePool.data.resize(2);
        textures.texturePool.data[1] = {};
        CHECK(manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries) == first);
        CHECK(Pipeline::constructions == compiled);
        // A changed CBUF selector must be checked before dereferencing old TIC0.
        cbufs[0][0].words[0] = 1;
        // TIC0 contents stay unchanged while the CBUF selects TIC1.
        textures.texturePool.data[1].textureType = Tic::TextureType::e3D;
        auto selected = manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries);
        CHECK(selected->metadata == T::Color3D);
        SetTic(textures, 15, 0x1000); // old slot is now fatal; selected path must not read it
        CHECK(manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries) == selected);
        textures.texturePool.data.resize(3);
        textures.texturePool.data[2].textureType = Tic::TextureType::e3D;
        cbufs[0][0].words[0] = 2;
        const int beforeEquivalent = Pipeline::constructions;
        CHECK(manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries) == selected);
        CHECK(Pipeline::constructions == beforeEquivalent);
        CHECK(PrepareDescriptors(ctx,textures,samplers,cbufs,selected,selected,true) == &quickUpdate);
        // Even the *same* pipeline must refresh its descriptors if the
        // texture backing/view epoch changed since the cached descriptor.
        CHECK(PrepareDescriptors(ctx,textures,samplers,cbufs,selected,selected,true,1,2) == &fullUpdate);
        cbufs[0][0].words[0] = 1;
        textures.integerFormat = 1;
        auto integerVariant = manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries);
        CHECK(integerVariant != selected);
        CHECK(!selected->CheckBindingMatch(integerVariant));
        CHECK(PrepareDescriptors(ctx,textures,samplers,cbufs,selected,integerVariant,false) == &fullUpdate);
        CHECK(PrepareDescriptors(ctx,textures,samplers,cbufs,selected,integerVariant,true) == &fullUpdate);
        samplers.comparison = 3;
        const auto compared = manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries);
        CHECK(manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries) == compared);
        CHECK(PrepareDescriptors(ctx,textures,samplers,cbufs,integerVariant,compared,false) == &fullUpdate);
        CHECK(PrepareDescriptors(ctx,textures,samplers,cbufs,compared,compared,false) == nullptr);
        // Actual dirty wrapper must refresh clean register state on metadata change.
        dirty::ManualDirtyState<PipelineState> state;
        StateUpdateBuilder builder;
        state.Update(ctx, textures, samplers, cbufs, builder);
        auto before = state.Get().pipeline;
        state.Update(ctx, textures, samplers, cbufs, builder);
        CHECK(state.Get().flushes == 1);
        textures.texturePool.data[1].textureType = Tic::TextureType::eCube;
        state.Update(ctx, textures, samplers, cbufs, builder);
        CHECK(state.Get().flushes == 2 && state.Get().pipeline != before);
        CHECK(state.Get().pipeline->metadata == T::ColorCube);
        CHECK(before->metadata == T::Color3D);
        // An attached GPU-dirty read must unwind rather than accept cached state.
        struct PendingRead {};
        cbufs[0][0].beforeRead = [] { throw PendingRead{}; };
        bool unwound{};
        try { state.Update(ctx, textures, samplers, cbufs, builder); } catch (const PendingRead&) { unwound=true; }
        CHECK(unwound);
        state.MarkDirty(true); // executor retry boundary invalidates after submit
        state.Update(ctx, textures, samplers, cbufs, builder);
        CHECK(state.Get().pipeline->metadata == T::ColorCube);
        // Safe compile inputs must handle an unbind and reject a too-short view
        // before touching BufferView memory/locks. Raw handle zero remains TIC0.
        SetTic(textures, 1, 0x1000);
        auto &word = cbufs[0][0];
        word.view.bound = false;
        const int readsBeforeUnbind = word.reads;
        Pipeline *unbound{};
        CHECK(!Throws([&] { unbound = manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries); }));
        CHECK(word.reads == readsBeforeUnbind);
        CHECK(unbound && unbound->metadata == T::Color2D);
        word.view.bound = true;
        word.view.size = 2;
        const int readsBeforeShort = word.reads;
        CHECK(Throws([&] { manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries); }));
        CHECK(word.reads == readsBeforeShort);
        word.view.size = 256;
        CHECK(manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries)->metadata == T::ColorCube);
#ifdef TIC_VULKAN_LIFETIME
        CHECK(runtime.Complete());
#endif
        // Genuine scalar values folded into code still require new variants.
        testScalarFold = true;
        PipelineManager folded;
        auto foldedA = folded.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries);
        cbufs[0][1].words[1] = 17;
        CHECK(folded.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries) != foldedA);
        cbufs[0][1].words[1] = 0;
        CHECK(folded.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries) == foldedA);
        testScalarFold = false;
        // Same TIC index, different high TSC bits: current handle still matters.
        samplers.bySampler[1]=1; samplers.bySampler[2]=3;
        auto samplerBaseline=manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries);
        cbufs[0][0].words[0]=0x100001;
        auto samplerOne=manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries);
        cbufs[0][0].words[0]=0x200001;
        auto samplerTwo=manager.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries);
        CHECK(samplerOne!=samplerTwo && samplerTwo==samplerBaseline);
        // Handle source split across two CBUFs, with a secondary left shift.
        testSplitHandle=true;
        PipelineManager split;
        cbufs[0][0].words[0]=1;
        cbufs[0][1].words[1]=1;
        auto splitOne=split.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries);
        cbufs[0][1].words[1]=2;
        CHECK(split.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries)!=splitOne);
        cbufs[0][1].words[1]=1;
        CHECK(split.FindOrCreate(ctx,textures,samplers,cbufs,key,binaries)==splitOne);
        cbufs[0][1].words[1]=0;
        testSplitHandle=false;
        // Host CPU-cost control: equivalent handles, clean register state,
        // one shared CBUF word per query source, no compilations/flushes.
        textures.texturePool.data[2].textureType = Tic::TextureType::eCube;
        const int beforeHot = Pipeline::constructions;
        const int beforeReads = cbufs[0][0].reads;
        const int beforeFlushes = state.Get().flushes;
        const auto begin = std::chrono::steady_clock::now();
        constexpr int iterations=20000;
        for (int n=0;n<iterations;++n) {
            cbufs[0][0].words[0] = 1 + (n&1);
            state.Update(ctx,textures,samplers,cbufs,builder);
        }
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
        CHECK(Pipeline::constructions == beforeHot);
        CHECK(state.Get().flushes == beforeFlushes);
        CHECK(cbufs[0][0].reads-beforeReads == iterations);
        std::printf("host refresh: %d equivalent-handle draws, 0 compiles, 0 flushes, 1 CBUF word/draw, %.1f ns/draw (facade, not device frametime)\n",iterations,double(elapsed)/iterations);
        std::printf("cache probe: %d checks, %d failures; variants stable and reused\n", checks, failures);
        return failures ? 1 : 0;
    }
    const std::array<T, 9> expected{T::Color1D,T::Color2D,T::Color3D,T::ColorCube,T::ColorArray1D,T::ColorArray2D,T::Buffer,T::Color2D,T::ColorArrayCube};
    for (u32 type = 0; type <= 8; ++type) {
        SetTic(textures, type, 0xFFFFFFFFFFFFULL);
        CHECK(textures.GetTextureType(ctx, 0) == expected[type]);
    }
    for (u32 type = 9; type <= 15; ++type) {
        for (u64 address : {0ULL, 0x8000ULL, 1ULL << 40, 0xFFFFFFFFFFFFULL}) {
            SetTic(textures, type, address);
            CHECK(textures.GetTextureType(ctx, 0) == T::Color2D);
            for (T shader : expected) {
                Desc desc{shader};
                ctx.gpu.traits.supportsNullDescriptor = true;
                auto sampled = GetTextureBinding(ctx, desc, samplers, textures, {.raw = 0}, vk::PipelineStageFlagBits::eFragmentShader, src, dst);
                CHECK(sampled.first.imageView == 0 && sampled.second == nullptr);
                // Invalid target is not evidence that storage reads/stores may be discarded.
                CHECK(Throws([&] { GetImageBinding(ctx, desc, textures, {.raw = 0}, vk::PipelineStageFlagBits::eComputeShader, src, dst); }));
                ctx.gpu.traits.supportsNullDescriptor = false;
                CHECK(Throws([&] { GetTextureBinding(ctx, desc, samplers, textures, {.raw = 0}, vk::PipelineStageFlagBits::eFragmentShader, src, dst); }));
                CHECK(Throws([&] { GetImageBinding(ctx, desc, textures, {.raw = 0}, vk::PipelineStageFlagBits::eComputeShader, src, dst); }));
            }
        }
        for (u64 address : {0x1000ULL, 0x1FFFULL}) {
            SetTic(textures, type, address);
            CHECK(Throws([&] { textures.GetTextureType(ctx, 0); }));
            NullTextureReason reason{};
            CHECK(Throws([&] { textures.GetTexture(ctx, 0, T::Color2D, &reason); }));
        }
        SetTic(textures, type, 0x2000); // immediately beyond mapping
        CHECK(textures.GetTextureType(ctx, 0) == T::Color2D);
    }
    // Regression: a reserved/unmapped TIC and an out-of-pool TSC must never
    // produce a combined descriptor with a null sampler (VUID 00325).
    SetTic(textures, 15, 0x8000);
    ctx.gpu.traits.supportsNullDescriptor = true;
    for (T type : expected) {
        Desc desc{type};
        samplers.samplerPoolSize = 1;
        auto fallback = GetTextureBinding(ctx, desc, samplers, textures, {.raw = 0xFFE00000U}, vk::PipelineStageFlagBits::eFragmentShader, src, dst);
        CHECK(fallback.first.imageView == 0 && fallback.second == nullptr);
        CHECK(fallback.first.sampler == 23);
        samplers.samplerPoolSize = 4096;
        auto bound = GetTextureBinding(ctx, desc, samplers, textures, {.raw = 0}, vk::PipelineStageFlagBits::eFragmentShader, src, dst);
        CHECK(bound.first.sampler == 17); // retain a valid guest sampler
    }
    samplers.samplerPoolSize = 1;
    ctx.gpu.traits.supportsNullDescriptor = false;
    Desc noNull{T::Color2D};
    CHECK(Throws([&] { GetTextureBinding(ctx, noNull, samplers, textures, {.raw = 0xFFE00000U}, vk::PipelineStageFlagBits::eFragmentShader, src, dst); }));
    samplers.samplerPoolSize = 4096;
    // Sparse mappings have no resident CPU backing according to LookupBlock.
    ctx.as.gmmu.blockSegmentTable.entries[0x3000] = {0x3000, backing.data(), backing.size(), {true}};
    SetTic(textures, 15, 0x3000);
    CHECK(textures.GetTextureType(ctx, 0) == T::Color2D);
    // No cached null result: replacing the entry restores every supported type.
    for (u32 type = 0; type <= 8; ++type) {
        SetTic(textures, type, 0x1000);
        CHECK(textures.GetTextureType(ctx, 0) == expected[type]);
        NullTextureReason reason{};
        CHECK(textures.GetTexture(ctx, 0, expected[type], &reason) == &textures.validView);
        CHECK(reason == NullTextureReason::None);
    }
    // An existing valid-view cache must re-check a changed TIC after the channel
    // sequence changes (the normal I2M/submission invalidation boundary).
    SetTic(textures, 1, 0x1000);
    textures.textureHeaderCache.resize(1);
    textures.textureHeaderCache[0] = {textures.texturePool.data[0], T::Color2D, &textures.validView, ctx.channelCtx.channelSequenceNumber};
    SetTic(textures, 15, 0x8000);
    ++ctx.channelCtx.channelSequenceNumber;
    NullTextureReason invalidReason{};
    CHECK(textures.GetTexture(ctx, 0, T::Color2D, &invalidReason) == textures.nullTextureView.get());
    CHECK(invalidReason != NullTextureReason::None && invalidReason != NullTextureReason::Unmapped);
    SetTic(textures, 15, 0x1000);
    CHECK(Throws([&] { textures.GetTexture(ctx, 0, T::Color2D, &invalidReason); }));
    textures.textureHeaderCache.clear();
    // A reserved target becomes fatal when its base is mapped, without changing TIC bytes.
    SetTic(textures, 15, 0x8000);
    CHECK(textures.GetTextureType(ctx, 0) == T::Color2D);
    ctx.as.gmmu.blockSegmentTable.entries[0x8000] = {0x8000, backing.data(), backing.size(), {false}};
    CHECK(Throws([&] { textures.GetTextureType(ctx, 0); }));
    ctx.gpu.traits.supportsNullDescriptor = true;
    Desc cube{T::ColorCube};
    auto sentinel = GetTextureBinding(ctx, cube, samplers, textures, {.raw = NullBindlessHandleRaw}, vk::PipelineStageFlagBits::eFragmentShader, src, dst);
    CHECK(sentinel.first.imageView == 0 && sentinel.first.sampler != 0);
    auto storageSentinel = GetImageBinding(ctx, cube, textures, {.raw = NullBindlessHandleRaw}, vk::PipelineStageFlagBits::eComputeShader, src, dst);
    CHECK(storageSentinel.first.imageView == 0 && storageSentinel.second == nullptr);
    CHECK(ctx.executor.attachments == 0); // no writable/sample stand-in attached
    CHECK(textures.validView.texture->writes == 0);
    std::printf("TIC executable checks: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
