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
struct TextureView { struct Texture { bool replaced{}; } *texture; };
struct AddressSpace {
    struct Gmmu {
        span<u8> bytes;
        std::pair<span<u8>, size_t> LookupBlock(u64 offset) { return {bytes, offset}; }
    } gmmu;
};
struct InterconnectContext {
    struct Channel { u64 channelSequenceNumber{}; AddressSpace *asCtx{}; } channelCtx;
};
struct TexturePoolState {
    struct Registers { struct HeaderPool { u64 offset{}; u32 maximumIndex{}; } texHeaderPool; } *engine;
    span<TextureImageControl> textureHeaders;
    u64 bindingGeneration{};
    void Flush(InterconnectContext &ctx);
};
struct Pool {
    span<TextureImageControl> textureHeaders;
    u64 bindingGeneration{1};
    unsigned reads{};
    Pool &UpdateGet(InterconnectContext &) { ++reads; return *this; }
};
struct Textures {
    Pool texturePool;
    struct CacheEntry { TextureImageControl tic; TextureView *view; u64 sequenceNumber; u64 bindingGeneration; };
    std::vector<CacheEntry> textureHeaderCache;
    Shader::TextureComponentType GetTextureComponentType(InterconnectContext &ctx, u32 index);
    u64 GetBindingGeneration(InterconnectContext &ctx);
    TextureView *GetCached(InterconnectContext &ctx, u32 index) {
#include "cache.inc"
        return nullptr;
    }
};
#include "components.inc"
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
        Textures textures{.texturePool = {headers}};
        AddressSpace as{{span<u8>{reinterpret_cast<u8 *>(headers.data()), headers.size() * sizeof(TextureImageControl)}}};
        InterconnectContext ctx{{0, &as}};
        TexturePoolState::Registers registers{{0, 0}};
        TexturePoolState pool{&registers};
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
        std::cout << "PASS production TIC signedness, stencil aspects, pool cache refresh and numeric bundle round-trip\n";
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
