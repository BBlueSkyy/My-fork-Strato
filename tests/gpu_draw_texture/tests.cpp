#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <span>

using i32 = std::int32_t;
using i64 = std::int64_t;
using u32 = std::uint32_t;
struct FakeSampler;
struct GPU;
struct FenceCycle {};
#define TRACE_EVENT(...)
#define LOGW(...)
namespace vk {
    struct Extent2D { u32 width{}, height{}; };
    struct Offset2D { i32 x{}, y{}; };
    struct Rect2D { Offset2D offset; Extent2D extent; };
    enum class PipelineStageFlagBits { eAllGraphics, eColorAttachmentOutput, eFragmentShader };
    using PipelineStageFlags = PipelineStageFlagBits;
    enum class Filter { eNearest, eLinear };
    enum class SamplerMipmapMode { eNearest, eLinear };
    using Sampler = u32;
    using RenderPass = u32;
    struct ConditionalRenderingBeginInfoEXT { u32 buffer{}; std::size_t offset{}; };
    namespace raii { struct CommandBuffer { bool render{true}; void beginConditionalRenderingEXT(ConditionalRenderingBeginInfoEXT info) { render = info.buffer != 0; } void endConditionalRenderingEXT() { render = true; } }; }
    namespace raii { using Sampler = FakeSampler; }
}
struct TextureSamplerControl {
    enum class Filter : u32 { Nearest = 1, Linear = 2 };
    enum class MipFilter : u32 { None = 1, Nearest = 2, Linear = 3 };
};
#include "filters.inc"
namespace engine {
#include "registers.inc"
    static_assert(sizeof(DrawTexture) == 0x30);
    static_assert(offsetof(DrawTexture, samplerIndex) == 0x20);
    static_assert(offsetof(DrawTexture, srcY) == 0x2C);
    struct WindowOrigin { bool lowerLeft{}, flipY{}; };
    struct SurfaceClip {
        struct { i32 x{}; u32 width{}; } horizontal;
        struct { i32 y{}; u32 height{}; } vertical;
    };
    struct Scissor {
        bool enable{};
        struct { i32 xMin{}, xMax{}; } horizontal;
        struct { i32 yMin{}, yMax{}; } vertical;
    };
    struct SamplerBinding { enum class Value { Independently, ViaHeaderBinding }; Value value; };
}
namespace Shader { enum class TextureType { Color2D }; }
struct Texture {
    vk::Extent2D dimensions;
    bool dirty{};
    void MarkGpuDirty(int &) { dirty = true; }
};
struct TextureView : std::enable_shared_from_this<TextureView> {
    std::shared_ptr<Texture> texture;
    struct { u32 baseMipLevel{}; } range;
};
struct FakeSampler { u32 index; u32 operator*() const { return index + 100; } };
struct Executor {
    template<typename Operation> void RunWithTextureBackingRetry(Operation &&operation) { operation(); }
    int usageTracker{}, pipelineChanges{}, blits{};
    GPU *gpu{};
    std::vector<std::shared_ptr<void>> dependencies;
    std::vector<TextureView *> attached;
    vk::Rect2D area{};
    void AttachDependency(std::shared_ptr<void> value) { dependencies.push_back(value); }
    void AttachTexture(TextureView *view) { attached.push_back(view); }
    void AddCheckpoint(const char *) {}
    void NotifyPipelineChange();
    template <class F, class I, class C>
    void AddSubpass(F function, vk::Rect2D rect, I sampled, std::initializer_list<int>,
                    C colors, std::initializer_list<int>, bool, vk::PipelineStageFlags,
                    vk::PipelineStageFlags, std::function<int(u32)> hook = {}, std::span<TextureView *> writers = {}) {
        if (hook) hook(0);
        if (sampled[0] != attached[0] || colors[0] != attached[1])
            throw std::runtime_error("incorrect Vulkan resources");
        area = rect;
        vk::raii::CommandBuffer commandBuffer;
        function(commandBuffer, std::shared_ptr<FenceCycle>{}, *gpu, 0, 0);
        for (auto view : writers) if (view) view->texture->MarkGpuDirty(usageTracker);
    }
};
struct Rect { float width, height, x, y; };
void CountBlit(GPU &, bool);
struct BlitRecorder {
    Rect src{}, dst{};
    vk::Extent2D srcDims{}, dstDims{};
    vk::Sampler sampler{};
    std::optional<vk::Rect2D> scissor;
    template<class G, class F>
    void Blit(G &, Rect source, Rect destination, vk::Extent2D sourceDimensions,
              vk::Extent2D destinationDimensions, bool, TextureView *, TextureView *,
              F callback, vk::Sampler selectedSampler,
              std::optional<vk::Rect2D> selectedScissor = std::nullopt) {
        src = source; dst = destination;
        srcDims = sourceDimensions; dstDims = destinationDimensions;
        sampler = selectedSampler;
        scissor = selectedScissor;
        callback([&](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu, vk::RenderPass, u32) { CountBlit(gpu, commandBuffer.render); });
    }
};
// The helper boundary records arguments; it does not emulate sampling or Vulkan.
struct GPU { struct { BlitRecorder blitHelperShader; } helperShaders; int *blits{}; bool queryActive{}; int samples{}; };
void Executor::NotifyPipelineChange() { pipelineChanges++; gpu->queryActive = false; }
struct Context { GPU gpu; Executor executor; Context() { executor.gpu = &gpu; gpu.blits = &executor.blits; }
    Context(Context &&other) : gpu{std::move(other.gpu)}, executor{std::move(other.executor)} { executor.gpu = &gpu; gpu.blits = &executor.blits; } };
void CountBlit(GPU &gpu, bool enabled) { if (enabled) { ++*gpu.blits; if (gpu.queryActive) gpu.samples++; } }
using InterconnectContext = Context;
struct Samplers {
    struct Pool {
        struct State { bool didUseTexHeaderBinding{}; } state;
        const State &Get() { return state; }
    } samplerPool;
    FakeSampler sampler{};
    void Update(Context &, bool value) { samplerPool.state.didUseTexHeaderBinding = value; }
    FakeSampler *GetSampler(Context &, u32 samplerIndex, u32 textureIndex);
    FakeSampler *GetSampler(Context &, u32 samplerIndex) {
        sampler.index = samplerIndex;
        return &sampler;
    }
};
#include "sampler_binding.inc"
struct Textures {
    std::shared_ptr<TextureView> source;
    u32 selectedIndex{};
    TextureView *GetTexture(Context &, u32 index, Shader::TextureType) {
        selectedIndex = index;
        return source.get();
    }
};
struct ActiveState {
    std::shared_ptr<TextureView> destination;
    size_t selectedTarget{};
    std::shared_ptr<TextureView> GetColorRenderTargetForClear(Context &, size_t index) {
        selectedTarget = index;
        return destination;
    }
};
struct Queries {
    enum class CounterType { Occulusion };
    int PrepareDraw(Context &ctx, CounterType, bool enabled, u32) { ctx.gpu.queryActive = enabled; return 0; }
};
struct Maxwell3D {
    Context ctx;
    struct RenderConditionBinding { bool enabled{}; u32 predicate{1}; struct Binding { u32 buffer; std::size_t offset; }; Binding Resolve(GPU &) const { return {predicate, 0}; } };
    RenderConditionBinding activeRenderCondition{};
    Queries queries;
    u32 sampleCounterEnable{};
    void RefreshRenderCondition() {}
    Samplers samplers;
    Textures textures;
    ActiveState activeState;
    struct { engine::Scissor scissor0; } clearEngineRegisters;
    engine::SamplerBinding samplerBinding{engine::SamplerBinding::Value::ViaHeaderBinding};
    void DrawTexture(const engine::DrawTexture &, const engine::WindowOrigin &,
                     const engine::SurfaceClip &, size_t);
};
#include "production.inc"

static void Check(bool condition, const char *name) {
    if (!condition) throw std::runtime_error(name);
}
static Maxwell3D Fixture() {
    Maxwell3D gpu;
    gpu.textures.source = std::make_shared<TextureView>();
    gpu.textures.source->texture = std::make_shared<Texture>(vk::Extent2D{800, 480});
    gpu.activeState.destination = std::make_shared<TextureView>();
    gpu.activeState.destination->texture = std::make_shared<Texture>(vk::Extent2D{1280, 720});
    return gpu;
}
static engine::DrawTexture Parameters() {
    return {0, 720 * 4096, 1280 * 4096, 720 * 4096,
            0xA0000000LL, -0xAAAAAAABLL, 0, 8, 0, 480 * 4096};
}
int main() {
    int failures{};
    auto test = [&](const char *name, auto body) {
        try { body(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &e) { failures++; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("zero GPU predicate suppresses DRAW_TEXTURE", [] {
        auto gpu = Fixture(); gpu.ctx.executor.gpu = &gpu.ctx.gpu; gpu.ctx.gpu.blits = &gpu.ctx.executor.blits;
        gpu.activeRenderCondition = {true, 0};
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        Check(gpu.ctx.executor.blits == 0, "false query predicate rendered a textured rectangle");
    });
    test("DRAW_TEXTURE contributes samples only while enabled", [] {
        auto gpu = Fixture(); gpu.sampleCounterEnable = 1;
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        Check(gpu.ctx.gpu.samples == 1, "enabled helper draw was not counted");
        gpu.sampleCounterEnable = 0;
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        Check(gpu.ctx.gpu.samples == 1, "disabled helper draw contributed samples");
    });
    test("explicit TSC0 with TIC8 and linked shader binding", [] {
        auto gpu = Fixture();
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 2);
        Check(gpu.ctx.gpu.helperShaders.blitHelperShader.sampler == 100, "DRAW_TEXTURE selected TSC8 instead of explicit TSC0");
        Check(gpu.textures.selectedIndex == 8 && gpu.activeState.selectedTarget == 2, "wrong source or color target");
    });
    test("captured rectangle transports the full image", [] {
        auto gpu = Fixture();
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        const auto &blit = gpu.ctx.gpu.helperShaders.blitHelperShader;
        Check(blit.dst.x == 0 && blit.dst.y == 0 && blit.dst.width == 1280 && blit.dst.height == 720, "incorrect destination geometry");
        Check(blit.src.x == 0 && blit.src.y == 480 && blit.src.width == 800 && std::abs(blit.src.height + 480) < 0.001f, "incorrect gradients or source geometry");
        Check(gpu.ctx.executor.blits == 1 && gpu.activeState.destination->texture->dirty && gpu.ctx.executor.pipelineChanges == 1, "blit or state restoration missing");
    });
    test("normal shader binding still follows the linked flag", [] {
        Context ctx; Samplers samplers;
        samplers.Update(ctx, true);
        Check(samplers.GetSampler(ctx, 0, 8)->index == 8, "linked shader binding changed");
        Check(samplers.GetSampler(ctx, 0)->index == 0, "explicit fixed-function binding changed");
        Check(samplers.GetSampler(ctx, 0, 8)->index == 8, "explicit lookup mutated shader binding");
        samplers.Update(ctx, false);
        Check(samplers.GetSampler(ctx, 0, 8)->index == 0, "independent shader binding changed");
    });
    test("base mip dimensions are used for UV normalization", [] {
        auto gpu = Fixture();
        gpu.textures.source->texture->dimensions = {1600, 960};
        gpu.textures.source->range.baseMipLevel = 1;
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        Check(gpu.ctx.gpu.helperShaders.blitHelperShader.srcDims.width == 800 && gpu.ctx.gpu.helperShaders.blitHelperShader.srcDims.height == 480, "UVs normalized against parent image instead of selected mip");
    });
    test("triangle Y flip does not translate the fixed-function window rectangle", [] {
        auto gpu = Fixture();
        gpu.DrawTexture(Parameters(), {true, true}, {{0, 1280}, {0, 720}}, 0);
        Check(gpu.ctx.executor.blits == 1 && gpu.ctx.gpu.helperShaders.blitHelperShader.dst.y == 0, "triangle flip moved the rectangle off screen");
    });
    test("signed extents preserve source and destination direction", [] {
        auto gpu = Fixture(); auto params = Parameters();
        params.dstX = 1280 * 4096; params.dstY = 0;
        params.dstWidth = -1280 * 4096; params.dstHeight = -720 * 4096;
        params.srcX = 800 * 4096; params.srcY = 0;
        gpu.DrawTexture(params, {true, false}, {{0, 1280}, {0, 720}}, 0);
        const auto &blit = gpu.ctx.gpu.helperShaders.blitHelperShader;
        Check(gpu.ctx.executor.blits == 1 && gpu.ctx.executor.area.extent.width == 1280 && gpu.ctx.executor.area.extent.height == 720, "negative extent rejected or clipped incorrectly");
        Check(blit.dst.width == -1280 && blit.dst.height == -720 && blit.src.width == -800 && blit.src.height == 480, "signed gradients or extent direction changed");
    });
    test("unavailable destination cannot enqueue a blit", [] {
        auto gpu = Fixture(); gpu.activeState.destination.reset();
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        Check(gpu.ctx.executor.blits == 0 && gpu.ctx.executor.attached.empty(), "missing target was drawn");
    });
    test("source view survives pool eviction until cycle completion", [] {
        auto gpu = Fixture();
        std::weak_ptr<TextureView> source = gpu.textures.source;
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        gpu.textures.source.reset();
        Check(!source.expired(), "queued source was destroyed before GPU completion");
        gpu.ctx.executor.dependencies.clear();
        Check(source.expired(), "source leaked past the cycle");
    });
    test("same-image feedback is rejected", [] {
        auto gpu = Fixture();
        gpu.activeState.destination->texture = gpu.textures.source->texture;
        gpu.DrawTexture(Parameters(), {true, false}, {{0, 1280}, {0, 720}}, 0);
        Check(gpu.ctx.executor.blits == 0, "illegal same-image feedback");
    });
    test("empty rectangle does not dirty the target", [] {
        auto gpu = Fixture(); auto params = Parameters(); params.dstWidth = 0;
        gpu.DrawTexture(params, {true, false}, {{0, 1280}, {0, 720}}, 0);
        Check(!gpu.activeState.destination->texture->dirty && gpu.ctx.executor.blits == 0, "empty draw changed target");
    });
    test("valid sampler filters are preserved", [] {
        Check(ConvertSamplerFilter(TextureSamplerControl::Filter::Nearest) == vk::Filter::eNearest, "nearest changed");
        Check(ConvertSamplerFilter(TextureSamplerControl::Filter::Linear) == vk::Filter::eLinear, "linear changed");
        Check(ConvertSamplerMipFilter(TextureSamplerControl::MipFilter::None) == vk::SamplerMipmapMode::eNearest, "no-mipmap changed");
        Check(ConvertSamplerMipFilter(TextureSamplerControl::MipFilter::Linear) == vk::SamplerMipmapMode::eLinear, "linear mipmap changed");
    });
    test("surface clip and scissor0 constrain writes without stretching UVs", [] {
        auto gpu = Fixture();
        gpu.clearEngineRegisters.scissor0 = {true, {200, 600}, {100, 300}};
        gpu.DrawTexture(Parameters(), {true, false}, {{100, 800}, {50, 720}}, 0);
        const auto &area = gpu.ctx.executor.area;
        Check(area.offset.x == 200 && area.offset.y == 420 && area.extent.width == 400 && area.extent.height == 200, "guest scissor0 or lower-left transform ignored");
        const auto &blit = gpu.ctx.gpu.helperShaders.blitHelperShader;
        Check(blit.scissor && blit.scissor->offset.x == 200 && blit.scissor->extent.width == 400, "render area was not bound as the Vulkan scissor");
        Check(blit.dst.width == 1280 && blit.src.width == 800, "clipping stretched texture coordinates");
    });
    test("disabled scissor0 still respects surface clip", [] {
        auto gpu = Fixture();
        auto params = Parameters(); params.dstY = 0;
        gpu.DrawTexture(params, {}, {{64, 640}, {32, 360}}, 0);
        const auto &area = gpu.ctx.executor.area;
        Check(area.offset.x == 64 && area.offset.y == 32 && area.extent.width == 640 && area.extent.height == 360, "surface clip ignored");
    });
    test("reserved zero sampler filters have defined conversion", [] {
        Check(ConvertSamplerFilter(static_cast<TextureSamplerControl::Filter>(0)) == vk::Filter::eNearest, "zero filter conversion");
        Check(ConvertSamplerMipFilter(static_cast<TextureSamplerControl::MipFilter>(0)) == vk::SamplerMipmapMode::eNearest, "zero mip conversion");
    });
    return failures ? 1 : 0;
}
