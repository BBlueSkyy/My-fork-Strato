// SPDX-License-Identifier: MPL-2.0
#include <vulkan/vulkan.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

using u32 = uint32_t;
template<class T> using span = std::span<T>;
static void Check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
namespace texture { enum class RenderPassUsage { None, RenderTarget, Sampled }; }
struct Texture {
    bool guest{true}, everUsedAsRt{};
    struct Format { vk::ImageAspectFlags vkAspect{vk::ImageAspectFlagBits::eColor}; } storage;
    Format *format{&storage};
    texture::RenderPassUsage lastRenderPassUsage{};
    u32 lastRenderPassIndex{};
    vk::PipelineStageFlags pendingStageMask{}, readStageMask{};
    void RecordGpuWrite() {}
    void UpdateRenderPassUsage(u32, texture::RenderPassUsage);
    void PopulateReadBarrier(vk::PipelineStageFlagBits, vk::PipelineStageFlags &, vk::PipelineStageFlags &);
};
#include "texture.inc"
struct TextureView { Texture *texture; u32 mip{}; };
struct DescriptorUpdateInfo {};
struct Pipeline {
    struct Stage {
        vk::PipelineStageFlagBits stage{};
        u32 combinedImageSamplerDescTotalCount{};
    };
    struct { std::array<Stage, 5> stages{}; } descriptorInfo;
    uint8_t stageMask{};
    int updates{}, inherited{};
    bool compatible{true}, partialTouchesImages{}, partialReplacesWithNull{};
    DescriptorUpdateInfo update;
    void PopulateSampledImageReadBarriers(span<TextureView *>, vk::PipelineStageFlags &, vk::PipelineStageFlags &);
    bool CheckBindingMatch(Pipeline *) { return compatible; }
    void InheritDescriptorBufferViews(const Pipeline &) { ++inherited; }
    DescriptorUpdateInfo *SyncDescriptors(int, int, int, int, span<TextureView *> images,
                                        vk::PipelineStageFlags &src, vk::PipelineStageFlags &dst) {
        ++updates;
        // Full descriptor resolution calls GetTextureBinding for every element.
        PopulateBoundImages(images, src, dst);
        return &update;
    }
    DescriptorUpdateInfo *SyncDescriptorsQuickBind(int, int, int, int, int, span<TextureView *> images,
                                                  vk::PipelineStageFlags &src, vk::PipelineStageFlags &dst) {
        ++updates;
        if (partialReplacesWithNull) images[0] = nullptr;
        if (partialTouchesImages) PopulateBoundImages(images, src, dst);
        return &update;
    }
    void PopulateBoundImages(span<TextureView *> images, vk::PipelineStageFlags &src, vk::PipelineStageFlags &dst) {
        size_t index{};
        for (size_t i{}; i < descriptorInfo.stages.size(); ++i) {
            if (!(stageMask & (1 << i))) continue;
            auto &stage{descriptorInfo.stages[i]};
            for (u32 j{}; j < stage.combinedImageSamplerDescTotalCount; ++j) {
                auto *view{images[index++]};
                if (view && view->texture) view->texture->PopulateReadBarrier(stage.stage, src, dst);
            }
        }
    }
};
#include "pipeline.inc"

struct Draw {
    int ctx{}, samplers{}, textures{};
    struct { int boundConstantBuffers{}; bool quickBindEnabled{true}; std::optional<int> quickBind; } constantBuffers;
    Pipeline *activeDescriptorPipeline{};
    std::vector<TextureView *> activeDescriptorSetSampledImages;
    void Prepare(Pipeline *pipeline, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
#include "reuse.inc"
        activeDescriptorPipeline = pipeline;
    }
};

int main() {
    // An unchanged view selects mip N of a backing whose other mip has just
    // been rendered. View identity/descriptor bytes need no update, dependency does.
    Texture bloom;
    TextureView view{&bloom, 3};
    Pipeline pipeline;
    pipeline.stageMask = 1 << 4;
    pipeline.descriptorInfo.stages[4] = {vk::PipelineStageFlagBits::eFragmentShader, 1};
    Draw draw;
    draw.activeDescriptorPipeline = &pipeline;
    draw.activeDescriptorSetSampledImages = {&view};
    vk::PipelineStageFlags src{}, dst{};
    auto writer = [&] { bloom.UpdateRenderPassUsage(1, texture::RenderPassUsage::RenderTarget); src = {}; dst = {}; };
    auto dependency = [&] {
        Check(bool(src & vk::PipelineStageFlagBits::eColorAttachmentOutput), "cached sampled descriptor lost color-write source dependency");
        Check(bool(dst & vk::PipelineStageFlagBits::eFragmentShader), "cached sampled descriptor lost fragment-read dependency");
    };
    writer();
    draw.Prepare(&pipeline, src, dst);
    dependency();
    Check(pipeline.updates == 0, "unchanged descriptor unnecessarily rewritten");
    std::cout << "PASS unchanged descriptors after RT write\n";
    src = {}; dst = {};
    draw.Prepare(&pipeline, src, dst);
    Check(!src && !dst, "repeated read added a redundant dependency");
    std::cout << "PASS repeated reads without new writes\n";
    writer(); draw.constantBuffers.quickBind = 7;
    draw.Prepare(&pipeline, src, dst); dependency();
    Check(pipeline.updates == 1, "partial uniform update was not preserved");
    std::cout << "PASS uniform-only quick bind after RT write\n";
    writer(); pipeline.partialTouchesImages = true;
    draw.Prepare(&pipeline, src, dst); dependency();
    std::cout << "PASS partial image updates remain idempotent\n";
    writer(); draw.activeDescriptorPipeline = nullptr;
    draw.Prepare(&pipeline, src, dst); dependency();
    std::cout << "PASS full descriptor update\n";
    Pipeline compatible{pipeline};
    writer(); draw.constantBuffers.quickBind.reset();
    draw.Prepare(&compatible, src, dst); dependency();
    Check(compatible.inherited == 1, "matching pipeline did not inherit descriptor cache");
    std::cout << "PASS compatible pipeline descriptor reuse\n";

    // Exact stage and array element traversal, including a null descriptor.
    Texture depth;
    depth.storage.vkAspect = vk::ImageAspectFlagBits::eDepth;
    TextureView depthView{&depth};
    Pipeline stages;
    stages.stageMask = (1 << 0) | (1 << 4);
    stages.descriptorInfo.stages[0] = {vk::PipelineStageFlagBits::eVertexShader, 2};
    stages.descriptorInfo.stages[4] = {vk::PipelineStageFlagBits::eFragmentShader, 1};
    // Inactive stages must not advance the packed descriptor index.
    stages.descriptorInfo.stages[2] = {vk::PipelineStageFlagBits::eGeometryShader, 9};
    Draw arrays;
    arrays.activeDescriptorPipeline = &stages;
    arrays.activeDescriptorSetSampledImages = {nullptr, &depthView, &view};
    depth.UpdateRenderPassUsage(2, texture::RenderPassUsage::RenderTarget);
    writer(); arrays.Prepare(&stages, src, dst);
    Check(bool(src & vk::PipelineStageFlagBits::eEarlyFragmentTests) && bool(src & vk::PipelineStageFlagBits::eLateFragmentTests), "depth writer stages missing");
    Check(bool(dst & vk::PipelineStageFlagBits::eVertexShader) && bool(dst & vk::PipelineStageFlagBits::eFragmentShader), "shader stage/array offsets incorrect");
    std::cout << "PASS stage arrays, nulls and depth writers\n";

    // A changed descriptor must be resolved before visiting the retained views.
    // Replacing it with a null image must not consume the old image's pending read.
    writer(); draw.activeDescriptorPipeline = &pipeline;
    draw.constantBuffers.quickBind = 7;
    pipeline.partialReplacesWithNull = true;
    draw.Prepare(&pipeline, src, dst);
    Check(draw.activeDescriptorSetSampledImages[0] == nullptr, "partial update did not replace the sampled image");
    Check(!src && !dst && bool(bloom.pendingStageMask & vk::PipelineStageFlagBits::eFragmentShader), "read dependency consumed from an obsolete sampled view");
    std::cout << "PASS partial replacement uses resolved views\n";
}
