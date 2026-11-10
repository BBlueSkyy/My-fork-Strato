// SPDX-License-Identifier: MPL-2.0
#include <gpu/interconnect/common/null_texture_policy.h>
#include <functional>
#define VK_NULL_HANDLE 0
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <span>
template<class T> using span = std::span<T>;
using u32 = unsigned;
namespace vk {
enum class PipelineStageFlagBits { eFragmentShader };
using PipelineStageFlags = unsigned;
enum ImageUsageFlagBits { eStorage = 1 };
enum class ImageLayout { eGeneral };
enum class PipelineBindPoint { eGraphics, eCompute };
struct DescriptorImageInfo { int imageView{}; ImageLayout imageLayout{}; };
}
namespace skyline::gpu::interconnect {
void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class... T> std::runtime_error exception(const char *message, T...) { return std::runtime_error(message); }
struct TextureView;
struct Texture {
    unsigned writes{};
    bool stale{};
    void RequireAliasCoherence(unsigned) { if (stale) throw std::runtime_error("late attachment retry"); }
    static void ValidateAliasWrites(span<TextureView *>) {}
    void MarkGpuDirty(int &) { ++writes; }
    void TransitionLayout(vk::ImageLayout) {}
    void PopulateReadBarrier(vk::PipelineStageFlagBits, unsigned &, unsigned &) {}
};
struct TextureView {
    std::shared_ptr<Texture> texture{std::make_shared<Texture>()};
    unsigned usage{vk::eStorage};
    unsigned GetUsage() const { return usage; }
    int GetView(bool identity) const { Check(identity, "storage view lost identity mapping"); return 2; }
};
using DeferredStorageImageWrites = std::vector<TextureView *>;
struct DescriptorUpdateInfo {
    int copies{}, writes{}, bufferDescs{}, bufferDescDynamicBindings{}, pipelineLayout{}, descriptorSetLayout{};
    vk::PipelineBindPoint bindPoint{};
    int descriptorSetIndex{};
};
struct Allocator {
    bool fail{}; DescriptorUpdateInfo update;
    template<class T> T *EmplaceUntracked(T value) {
        if (fail) throw std::bad_alloc();
        update = value; return &update;
    }
};
struct CommandExecutor {
    unsigned tag{1};
    int usageTracker{};
    std::shared_ptr<Allocator> allocator{std::make_shared<Allocator>()};
    unsigned attachments{};
    void AttachTexture(TextureView *) { ++attachments; }
    struct RegionPublication { ~RegionPublication() {} };
    RegionPublication AcquireRegionPublication(span<TextureView *>) { return {}; } // Region protocol uses real Vulkan fixture.
    void CommitTextureWrites(span<TextureView *>);
};
#include "commit.inc"
struct InterconnectContext { CommandExecutor executor; struct { struct { bool supportsNullDescriptor{}; } traits; } gpu; };
union BindlessHandle { u32 raw{}; struct { u32 textureIndex : 20; u32 samplerIndex : 12; }; };
struct Textures {
    TextureView *view{}; bool retry{};
    NullTextureReason reason{}; unsigned lookups{};
    TextureView *GetTexture(InterconnectContext &, unsigned, int, NullTextureReason *nullReason) {
        ++lookups; *nullReason = reason;
        if (retry) throw std::runtime_error("late backing/readback retry");
        return view;
    }
};
struct Descriptor { int type{1}; bool is_written{true}; };
#include "binding.inc"
struct Ranges { int first(u32) const { return 0; } };
struct Pipeline {
    // Buffer-list retention is observed here; buffer preflight/bytes are covered
    // by gpu_region_executor and the real Vulkan mixed fixture.
    int constantBuffers{};
    unsigned retainedBufferLists{};
    void RetainDescriptorBufferUses(int) { ++retainedBufferLists; }
    struct { int copyDescs{}; } descriptorInfo;
    struct { std::shared_ptr<int> pipelineLayout{std::make_shared<int>(1)}, descriptorSetLayout{std::make_shared<int>(2)}; } compiledPipeline;
    u32 writeIdx{1}, bufferIdx{};
    Ranges writes, bufferDescs, bufferDescDynamicBindings;
    std::vector<TextureView *> pendingWrites;
    DescriptorUpdateInfo *Finish0(InterconnectContext &ctx, [[maybe_unused]] DeferredStorageImageWrites &storageWriters) {
#include "tail0.inc"
    }
    DescriptorUpdateInfo *Finish1(InterconnectContext &ctx, [[maybe_unused]] DeferredStorageImageWrites &storageWriters) {
#include "tail1.inc"
    }
    DescriptorUpdateInfo *Finish2(InterconnectContext &ctx, [[maybe_unused]] DeferredStorageImageWrites &storageWriters) {
#include "tail2.inc"
    }
};
void Resolve(InterconnectContext &ctx, Textures &textures, Descriptor desc, DeferredStorageImageWrites &writers) {
    unsigned src{}, dst{};
#if HAS_STORAGE_PREFLIGHT
    GetImageBinding(ctx, desc, textures, {}, vk::PipelineStageFlagBits::eFragmentShader, src, dst, writers);
#else
    (void)writers;
    GetImageBinding(ctx, desc, textures, {}, vk::PipelineStageFlagBits::eFragmentShader, src, dst);
#endif
}
void CheckNullStorageBinding() {
    for (bool supported : {false, true}) {
        for (auto reason : {NullTextureReason::Unmapped, NullTextureReason::UnsupportedFormat, NullTextureReason::PoolIndexOutOfRange}) {
            InterconnectContext ctx; ctx.gpu.traits.supportsNullDescriptor = supported;
            TextureView dummy; dummy.usage = 0; Textures textures{&dummy}; textures.reason = reason;
            DeferredStorageImageWrites writers; bool rejected{}; unsigned src{}, dst{};
            std::pair<vk::DescriptorImageInfo, TextureView *> binding;
            try { binding = GetImageBinding(ctx, Descriptor{}, textures, {}, vk::PipelineStageFlagBits::eFragmentShader, src, dst, writers); }
            catch (const std::runtime_error &) { rejected = true; }
            const bool shouldBindNull{supported && reason == NullTextureReason::Unmapped};
            Check(rejected != shouldBindNull, "storage null reason selected wrong fallback");
            if (shouldBindNull) Check(!binding.first.imageView && !binding.second && binding.first.imageLayout == vk::ImageLayout::eGeneral, "unmapped storage image lost null descriptor");
            Check(!ctx.executor.attachments && writers.empty() && !dummy.texture->writes, "storage fallback attached sampled dummy or queued phantom writer");
            ctx.executor.CommitTextureWrites(writers);
            Check(!dummy.texture->writes, "null storage operation published a dummy writer");
        }
        InterconnectContext ctx; ctx.gpu.traits.supportsNullDescriptor = supported;
        TextureView dummy; Textures textures{&dummy}; DeferredStorageImageWrites writers;
        bool rejected{}; unsigned src{}, dst{};
        try {
            auto binding = GetImageBinding(ctx, Descriptor{}, textures, {.raw = NullBindlessHandleRaw}, vk::PipelineStageFlagBits::eFragmentShader, src, dst, writers);
            Check(!binding.first.imageView && !binding.second, "null sentinel lost null storage descriptor");
        } catch (const std::runtime_error &) { rejected = true; }
        Check(rejected != supported && !textures.lookups && !ctx.executor.attachments && writers.empty(), "null sentinel touched texture pool or queued phantom writer");
    }
    std::puts("PASS: exact storage null policy, sentinel and nullDescriptor capability; no sampled dummy attachment or phantom writer");
}
void Run() {
    CheckNullStorageBinding();
    using Finish = DescriptorUpdateInfo *(Pipeline::*)(InterconnectContext &, DeferredStorageImageWrites &);
    for (Finish finish : {&Pipeline::Finish0, &Pipeline::Finish1, &Pipeline::Finish2}) {
        InterconnectContext ctx; Pipeline pipeline; TextureView image; Textures textures{&image};
        {
            DeferredStorageImageWrites writers; Resolve(ctx, textures, {}, writers);
            Check(!image.texture->writes, "image resolution published a writer before later descriptors completed");
            textures.retry = true;
            bool retried{}; try { Resolve(ctx, textures, {}, writers); } catch (const std::runtime_error &) { retried = true; }
            Check(retried && !image.texture->writes, "abandoned descriptor attempt published a phantom write");
        }
        textures.retry = false;
        DeferredStorageImageWrites writers; Resolve(ctx, textures, {}, writers);
        ctx.executor.allocator->fail = true;
        bool failed{}; try { (pipeline.*finish)(ctx, writers); } catch (const std::bad_alloc &) { failed = true; }
        Check(failed && !image.texture->writes && !pipeline.retainedBufferLists, "failed update-info allocation published a writer or retained list");
        ctx.executor.allocator->fail = false;
        auto *update{(pipeline.*finish)(ctx, writers)};
        Check(update && !image.texture->writes, "descriptor tail published before operation preflight");
        TextureView later; later.texture->stale=true;
        std::vector<TextureView *> outputs{&image,&later};
        bool lateRetry{};
        try { ctx.executor.CommitTextureWrites(outputs); } catch (const std::runtime_error &) { lateRetry=true; }
        Check(lateRetry && !image.texture->writes, "later stale attachment published an earlier writer");
        ctx.executor.CommitTextureWrites(writers);
        Check(image.texture->writes==1, "successful operation lost its writer");
        // Descriptor allocation/update is intentionally skipped on a retained draw.
        ctx.executor.CommitTextureWrites(writers);
        Check(image.texture->writes==2, "retained storage binding did not publish its later operation");
        image.texture->writes = 0; writers.clear();
        Resolve(ctx, textures, Descriptor{1, false}, writers);
        (pipeline.*finish)(ctx, writers);
        Check(!image.texture->writes, "read-only storage image became a writer");
    }
    std::puts("PASS: production storage binding and graphics/full/quick/compute allocation tails; late retry, allocation failure, deferred operation claims and retained storage bindings");
}
}
int main() {
    try { skyline::gpu::interconnect::Run(); }
    catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
