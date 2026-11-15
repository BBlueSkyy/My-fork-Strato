// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
using u8 = std::uint8_t; using u16 = std::uint16_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
namespace Shader {
#include "shader-type.inc"
}
namespace skyline::util { template<class T, class U> constexpr T BitCast(U value) { return std::bit_cast<T>(value); } }
#include <gpu/interconnect/common/tic.h>
#include <gpu/interconnect/common/null_texture_policy.h>
#include <gpu/interconnect/common/tic_validation_policy.h>
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
    struct State {
        std::span<TextureImageControl> textureHeaders;
        u64 GetPoolIova() const { return 0x4000; }
    } state{};
    // The production ManualDirtyState returns a persistent State reference.
    State &UpdateGet(InterconnectContext&) {
        state.textureHeaders = data;
        return state;
    }
};

// Test-only inert diagnostics: production LogTicProbe and TIC tracking are
// exercised separately by tests/gpu_tic_probe. The TIC guards and bindings
// below still execute the extracted production functions unchanged.
namespace tic_probe {
    inline bool ConsumeRecovered(u64, u32) { return false; }
}
static void LogTicProbe(InterconnectContext&, const Pool::State&, u32,
                        const TextureImageControl&, bool, const char*, u32) {}

struct Textures {
    struct CacheEntry { TextureImageControl tic{}; Shader::TextureType shaderType{}; TextureView *view{}; u64 sequenceNumber{}; };
    Pool texturePool;
    std::vector<CacheEntry> textureHeaderCache;
    std::shared_ptr<TextureView> nullTextureView;
    TextureView validView;
    TextureView *GetTexture(InterconnectContext&, u32, Shader::TextureType, NullTextureReason*);
    Shader::TextureType GetTextureType(InterconnectContext&, u32);
};
static std::shared_ptr<TextureView> CreateNullTexture(InterconnectContext&) { return std::make_shared<TextureView>(); }
#include "textures.inc"
struct Samplers {
    struct Sampler { u64 operator*() const { return 17; } } sampler;
    Sampler *GetNullSampler(InterconnectContext&) { return &sampler; }
    Sampler *GetSampler(InterconnectContext&, u32, u32) { return &sampler; }
};
#include "bindings.inc"
// Real cache-hit/transition functions; only pipeline construction is a facade.
// Capture precisely the descriptor metadata available when compilation occurs.
struct PackedPipelineState { int key{}; int shaderHashes{}; bool operator==(const PackedPipelineState&) const = default; };
struct KeyHash { size_t operator()(const PackedPipelineState &v) const { return v.key; } };
struct ConstantBufferSet {};
struct ShaderBinary {};
namespace engine { constexpr size_t PipelineCount = 1; }
struct PipelineStateBundle { void Reset(const PackedPipelineState&) {} };
struct RuntimeGraphicsPipelineStateAccessor {
    InterconnectContext &ctx; Textures &textures;
    RuntimeGraphicsPipelineStateAccessor(std::unique_ptr<PipelineStateBundle>, InterconnectContext &c, Textures &t, Samplers&, ConstantBufferSet&, const std::array<ShaderBinary, 1>&) : ctx(c), textures(t) {}
};
struct Pipeline {
    PackedPipelineState sourcePackedState;
    Shader::TextureType metadata;
    std::array<Pipeline*, 2> transitionCache{};
    Pipeline(InterconnectContext::Gpu&, RuntimeGraphicsPipelineStateAccessor &a, const PackedPipelineState &s) : sourcePackedState(s), metadata(a.textures.GetTextureType(a.ctx, 0)) {}
    Pipeline *LookupNext(const PackedPipelineState&);
};
struct PipelineManager {
    std::unordered_map<PackedPipelineState, std::unique_ptr<Pipeline>, KeyHash> map;
    Pipeline *FindOrCreate(InterconnectContext&, Textures&, Samplers&, ConstantBufferSet&, const PackedPipelineState&, const std::array<ShaderBinary, 1>&);
};
#include "cache.inc"
namespace compute {
struct Pipeline {
    Shader::TextureType metadata;
    Pipeline(InterconnectContext &c, Textures &t, Samplers&, ConstantBufferSet&, const PackedPipelineState&, const ShaderBinary&) : metadata(t.GetTextureType(c, 0)) {}
};
struct Manager {
    std::unordered_map<PackedPipelineState, std::unique_ptr<Pipeline>, KeyHash> map;
#include "compute-cache.inc"
};
}
}
}
using namespace skyline;
using namespace skyline::gpu::interconnect;
using T = Shader::TextureType;
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
int main(int argc, char**) {
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
        SetTic(textures, 6, 0x1000);
        CHECK(textures.GetTextureType(ctx, 0) == T::Buffer);
        auto reused = manager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries);
        auto reusedCompute = computeManager.FindOrCreate(ctx, textures, samplers, cbufs, key, binaries[0]);
        // These expected-correctness assertions intentionally remain red: key and
        // transition caches do not track the descriptor-dependent specialization.
        CHECK(reused->metadata == T::Buffer);
        CHECK(first->LookupNext(key)->metadata == T::Buffer);
        auto otherKey = key; otherKey.key = 99;
        RuntimeGraphicsPipelineStateAccessor accessor{std::make_unique<PipelineStateBundle>(), ctx, textures, samplers, cbufs, binaries};
        Pipeline other{ctx.gpu, accessor, otherKey};
        other.transitionCache[0] = first;
        CHECK(other.LookupNext(key)->metadata == T::Buffer);
        CHECK(reusedCompute->metadata == T::Buffer);
        std::printf("cache probe: graphics/transition/compute retained type %u, live TIC type %u\n", unsigned(reused->metadata), unsigned(textures.GetTextureType(ctx, 0)));
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
