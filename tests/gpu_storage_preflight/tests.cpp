// SPDX-License-Identifier: MPL-2.0
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
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
struct Texture {
    unsigned writes{};
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
struct InterconnectContext {
    struct Executor {
        int usageTracker{};
        std::shared_ptr<Allocator> allocator{std::make_shared<Allocator>()};
        void AttachTexture(TextureView *) {}
    } executor;
};
struct BindlessHandle { u32 textureIndex{}; };
struct Textures {
    TextureView *view{}; bool retry{};
    TextureView *GetTexture(InterconnectContext &, unsigned, int) {
        if (retry) throw std::runtime_error("late backing/readback retry");
        return view;
    }
};
struct Descriptor { int type{1}; bool is_written{true}; };
#include "binding.inc"
struct Ranges { int first(u32) const { return 0; } };
struct Pipeline {
    struct { int copyDescs{}; } descriptorInfo;
    struct { std::shared_ptr<int> pipelineLayout{std::make_shared<int>(1)}, descriptorSetLayout{std::make_shared<int>(2)}; } compiledPipeline;
    u32 writeIdx{1}, bufferIdx{};
    Ranges writes, bufferDescs, bufferDescDynamicBindings;
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
void Run() {
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
        Check(failed && !image.texture->writes, "failed update-info allocation published a writer");
        ctx.executor.allocator->fail = false;
        auto *update{(pipeline.*finish)(ctx, writers)};
        Check(update && image.texture->writes == 1, "successful preflight lost its writer claim");
        image.texture->writes = 0; writers.clear();
        Resolve(ctx, textures, Descriptor{1, false}, writers);
        (pipeline.*finish)(ctx, writers);
        Check(!image.texture->writes, "read-only storage image became a writer");
    }
    // Keep the fallback function referenced in the negative control too.
    if (false) { InterconnectContext ctx; DeferredStorageImageWrites writers; CommitStorageImageWrites(ctx, writers); }
    std::puts("PASS: production storage binding and graphics/full/quick/compute allocation tails; late retry, allocation failure and successful claims");
}
}
int main() {
    try { skyline::gpu::interconnect::Run(); }
    catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
