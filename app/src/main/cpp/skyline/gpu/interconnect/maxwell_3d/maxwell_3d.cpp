// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cmath>
#include <bit>
#include <map>
#include <string>
#include <gpu/interconnect/command_executor.h>
#include <gpu/interconnect/conversion/quads.h>
#include <gpu/interconnect/common/state_updater.h>
#include <soc/gm20b/channel.h>
#include "common/utils.h"
#include "maxwell_3d.h"
#include "common.h"

namespace skyline::gpu::interconnect::maxwell3d {
    Maxwell3D::RenderConditionResources::RenderConditionResources(GPU &gpu)
        : scratch{gpu.memory.AllocateBuffer(sizeof(u32) * 5)},
          descriptorSet{gpu.helperShaders.conditionalCompareHelperShader.CreateDescriptorSet(gpu, scratch.vkBuffer)} {}

    Maxwell3D::Maxwell3D(GPU &gpu,
                         soc::gm20b::ChannelContext &channelCtx,
                         TrapManager &trap,
                         skyline::kernel::MemoryManager &memoryManager,
                         DirtyManager &manager,
                         const EngineRegisterBundle &registerBundle)
        : ctx{channelCtx, channelCtx.executor, gpu, trap, memoryManager},
          activeState{manager, registerBundle.activeStateRegisters},
          clearEngineRegisters{registerBundle.clearRegisters},
          constantBuffers{manager, registerBundle.constantBufferSelectorRegisters},
          samplers{manager, registerBundle.samplerPoolRegisters},
          samplerBinding{registerBundle.samplerBinding},
          textures{manager, registerBundle.texturePoolRegisters},
          sampleCounterEnable{registerBundle.sampleCounterEnable},
          directState{activeState.directState},
          queries{gpu} {
        ctx.executor.AddFlushCallback([this] {
            if (attachedDescriptorSets) {
                ctx.executor.AttachDependency(attachedDescriptorSets);
                attachedDescriptorSets = nullptr;
                activeDescriptorSet = nullptr;
            }

            activeDescriptorPipeline = nullptr;
            activeState.MarkAllDirty();
            constantBuffers.MarkAllDirty();
            samplers.MarkAllDirty();
            textures.MarkAllDirty();
            quadConversionBufferAttached = false;
            constantBuffers.DisableQuickBind();
        });

        ctx.executor.AddSubpassBoundaryCallback([this] {
            queries.Pause(ctx);
        });

        ctx.executor.AddPipelineChangeCallback([this] {
            queries.Pause(ctx);
            activeDescriptorPipeline = nullptr;
            activeState.MarkAllDirty();
            activeDescriptorSet = nullptr;
        });

        if (gpu.traits.supportsConditionalRendering)
            renderConditionResources = std::make_shared<RenderConditionResources>(gpu);
    }

    vk::DeviceSize Maxwell3D::UpdateQuadConversionBuffer(u32 count, u32 firstVertex) {
        vk::DeviceSize offset{conversion::quads::GetRequiredBufferSize(firstVertex, sizeof(u32))};
        vk::DeviceSize size{conversion::quads::GetRequiredBufferSize(count, sizeof(u32)) + offset};

        if (!quadConversionBuffer || quadConversionBuffer->size_bytes() < size) {
            quadConversionBuffer = std::make_shared<memory::Buffer>(ctx.gpu.memory.AllocateBuffer(util::AlignUp(size, PAGE_SIZE)));
            conversion::quads::GenerateQuadListConversionBuffer(quadConversionBuffer->cast<u32>().data(), firstVertex + count);
            quadConversionBufferAttached = false;
        }

        if (!quadConversionBufferAttached) {
            ctx.executor.AttachDependency(quadConversionBuffer);
            quadConversionBufferAttached = true;
        }

        return offset;
    }
    
    static vk::IndexType ConvertInlineIndexSize(engine::IndexBuffer::IndexSize size) {
    switch (size) {
      case engine::IndexBuffer::IndexSize::OneByte:
        return vk::IndexType::eUint8EXT;
      case engine::IndexBuffer::IndexSize::TwoBytes:
        return vk::IndexType::eUint16;
      case engine::IndexBuffer::IndexSize::FourBytes:
        return vk::IndexType::eUint32;
    }
    throw exception("Invalid inline index size: {}", static_cast<u32>(size));
}

     void Maxwell3D::DrawWithInlineIndex(engine::DrawTopology topology, bool transformFeedbackEnable,
                                     span<u8> indexData, engine::IndexBuffer::IndexSize indexSize,
                                     u32 count, u32 instanceCount) {
        ctx.executor.RunWithTextureBackingRetry([&] {
     TRACE_EVENT("gpu", "DrawWithInlineIndex", "count", count, "instanceCount", instanceCount);

       StateUpdateBuilder builder{*ctx.executor.allocator};
       vk::PipelineStageFlags srcStageMask{}, dstStageMask{};

    PrepareDraw(builder, topology, true, false, 0, count, srcStageMask, dstStageMask);

    inlineIndexBuffer = std::make_shared<memory::Buffer>(ctx.gpu.memory.AllocateBuffer(util::AlignUp(indexData.size_bytes(), PAGE_SIZE)));
    std::memcpy(inlineIndexBuffer->cast<u8>().data(), indexData.data(), indexData.size_bytes());
    ctx.executor.AttachDependency(inlineIndexBuffer);

    builder.SetIndexBuffer(BufferBinding{inlineIndexBuffer->vkBuffer, 0}, ConvertInlineIndexSize(indexSize));

    RefreshRenderCondition();
    auto stateUpdater{builder.Build()};

    struct DrawParams {
        StateUpdater stateUpdater;
        RenderConditionBinding renderCondition;
        u32 count;
        u32 instanceCount;
        bool transformFeedbackEnable;
    };
    auto *drawParams{ctx.executor.allocator->EmplaceUntracked<DrawParams>(DrawParams{stateUpdater,
        activeRenderCondition, count, instanceCount, ctx.gpu.traits.supportsTransformFeedback ? transformFeedbackEnable : false})};

    vk::Rect2D scissor{GetDrawScissor()};
    constantBuffers.ResetQuickBind();

    ctx.executor.AddCheckpoint("Before inline index draw");
    ctx.executor.AddSubpass([drawParams](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu, vk::RenderPass, u32) {
        drawParams->stateUpdater.RecordAll(gpu, commandBuffer);

        if (drawParams->renderCondition.enabled) {
            auto conditionBinding{drawParams->renderCondition.Resolve(gpu)};
            commandBuffer.beginConditionalRenderingEXT(vk::ConditionalRenderingBeginInfoEXT{
                .buffer = conditionBinding.buffer,
                .offset = conditionBinding.offset,
            });
        }

        if (drawParams->transformFeedbackEnable)
            commandBuffer.beginTransformFeedbackEXT(0, {}, {});
        commandBuffer.drawIndexed(drawParams->count, drawParams->instanceCount, 0, 0, 0);
        if (drawParams->transformFeedbackEnable)
            commandBuffer.endTransformFeedbackEXT(0, {}, {});

        if (drawParams->renderCondition.enabled)
            commandBuffer.endConditionalRenderingEXT();
    }, scissor, activeDescriptorSetSampledImages, {}, activeState.GetColorAttachments(), activeState.GetDepthAttachment(),
         !ctx.gpu.traits.quirks.relaxedRenderPassCompatibility, srcStageMask, dstStageMask,
         [this](u32 renderPassIndex) {
             return queries.PrepareDraw(ctx, Queries::CounterType::Occulusion, sampleCounterEnable != 0, renderPassIndex);
         });
          InvalidateRenderConditionAfterDraw(transformFeedbackEnable && ctx.gpu.traits.supportsTransformFeedback);
          ctx.executor.AddCheckpoint("After inline index draw");
     
        });
    }    
   
     vk::Rect2D Maxwell3D::GetClearScissor() {
        const auto &clearSurfaceControl{clearEngineRegisters.clearSurfaceControl};

        const auto &surfaceClip{clearEngineRegisters.surfaceClip};
        vk::Rect2D scissor{
            {surfaceClip.horizontal.x, surfaceClip.vertical.y},
            {surfaceClip.horizontal.width, surfaceClip.vertical.height}
        };

        auto rectIntersection{[](const vk::Rect2D &a, const vk::Rect2D &b) -> vk::Rect2D {
            vk::Offset2D maxOffset{std::max(a.offset.x, b.offset.x), std::max(a.offset.y, b.offset.y)};
            i32 signedWidth{std::min(a.offset.x + static_cast<i32>(a.extent.width), b.offset.x + static_cast<i32>(b.extent.width)) - maxOffset.x};
            i32 signedHeight{std::min(a.offset.y + static_cast<i32>(a.extent.height), b.offset.y + static_cast<i32>(b.extent.height)) - maxOffset.y};

            return {
                maxOffset,
                {static_cast<u32>(std::max(signedWidth, 0)),
                 static_cast<u32>(std::max(signedHeight, 0))}
            };
        }};

        if (clearSurfaceControl.useClearRect) {
            const auto &clearRect{clearEngineRegisters.clearRect};
            scissor = rectIntersection(scissor, {
                {clearRect.horizontal.xMin, clearRect.vertical.yMin},
                {static_cast<u32>(clearRect.horizontal.xMax - clearRect.horizontal.xMin), static_cast<u32>(clearRect.vertical.yMax - clearRect.vertical.yMin)}
            });
        }

        if (clearSurfaceControl.useScissor0 && clearEngineRegisters.scissor0.enable) {
            const auto &scissor0{clearEngineRegisters.scissor0};
            scissor = rectIntersection(scissor, {
                {scissor0.horizontal.xMin, scissor0.vertical.yMin},
                {static_cast<u32>(scissor0.horizontal.xMax - scissor0.horizontal.xMin), static_cast<u32>(scissor0.vertical.yMax - scissor0.vertical.yMin)}
            });
        }

        if (clearSurfaceControl.useViewportClip0) {
            const auto &viewportClip0{clearEngineRegisters.viewportClip0};
            scissor = rectIntersection(scissor, {
                {viewportClip0.horizontal.x0, viewportClip0.vertical.y0},
                {viewportClip0.horizontal.width, viewportClip0.vertical.height}
            });
        }

        return scissor;
    }

    vk::Rect2D Maxwell3D::GetDrawScissor() {
        const auto &surfaceClip{clearEngineRegisters.surfaceClip};
        vk::Rect2D scissor{{surfaceClip.horizontal.x, surfaceClip.vertical.y},
                           {surfaceClip.horizontal.width, surfaceClip.vertical.height}};

        auto colorAttachments{activeState.GetColorAttachments()};
        auto depthStencilAttachment{activeState.GetDepthAttachment()};
        auto depthStencilAttachmentSpan{depthStencilAttachment ? span<TextureView *>(depthStencilAttachment) : span<TextureView *>()};
        for (auto attachment : ranges::views::concat(colorAttachments, depthStencilAttachmentSpan)) {
            if (attachment) {
                scissor.extent.width = std::min(scissor.extent.width, static_cast<u32>(static_cast<i32>(attachment->texture->dimensions.width) - scissor.offset.x));
                scissor.extent.height = std::min(scissor.extent.height, static_cast<u32>(static_cast<i32>(attachment->texture->dimensions.height) - scissor.offset.y));
            }
        }

        return scissor;
    }

     namespace { void CaptureHdrTrace(const Maxwell3D *, Pipeline *, DescriptorUpdateInfo *, bool, ConstantBufferSet &, span<TextureView *>, span<TextureView *>, TextureView *, CommandExecutor &); }

    void Maxwell3D::PrepareDraw(StateUpdateBuilder &builder,
                                 engine::DrawTopology topology, bool indexed, bool estimateIndexBufferSize, u32 firstIndex, u32 count,
                                 vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
         Pipeline *oldPipeline{activeState.GetPipeline()};
         samplers.Update(ctx, samplerBinding.value == engine::SamplerBinding::Value::ViaHeaderBinding);
         activeState.Update(ctx, textures, samplers, constantBuffers.boundConstantBuffers,
                            builder,
                            indexed, topology, estimateIndexBufferSize, firstIndex, count,
                            srcStageMask, dstStageMask);
         Pipeline *pipeline{activeState.GetPipeline()};
         activeDescriptorSetSampledImages.resize(pipeline->GetTotalSampledImageCount());


         // Cached attachments and quick-bind descriptors can bypass AttachTexture.
         // Retry here, before storage write claims or draw index conversion.
         const auto requireCoherence{[&](span<TextureView *> views) {
             for (auto view : views)
                 if (view && view->texture->HasOverlaps())
                     view->texture->RequireAliasCoherence(ctx.executor.tag);
         }};
         requireCoherence(activeState.GetColorAttachments());
         if (auto depth{activeState.GetDepthAttachment()}; depth && depth->texture->HasOverlaps())
             depth->texture->RequireAliasCoherence(ctx.executor.tag);

         bool hdrFullRefresh{};
         auto *descUpdateInfo{[&]() -> DescriptorUpdateInfo * {
             if (activeDescriptorPipeline &&
                 ((activeDescriptorPipeline == pipeline) || activeDescriptorPipeline->CheckBindingMatch(pipeline)) &&
                 constantBuffers.quickBindEnabled) {
                 requireCoherence(activeDescriptorSetSampledImages);
                 if (activeDescriptorPipeline != pipeline)
                     pipeline->InheritDescriptorBufferViews(*activeDescriptorPipeline);
                 // If bindings between the old and new pipelines are the same we can reuse the descriptor sets given that quick bind is enabled (meaning that no buffer updates or calls to non-graphics engines have occurred that could invalidate them)
                 if (constantBuffers.quickBind)
                     // If only a single constant buffer has been rebound between draws we can perform a partial descriptor update
                     return pipeline->SyncDescriptorsQuickBind(ctx, constantBuffers.boundConstantBuffers, samplers, textures,
                                                               *constantBuffers.quickBind, activeDescriptorSetSampledImages,
                                                               srcStageMask, dstStageMask);
                 else
                     return nullptr;
             } else {
                 // If bindings have changed or quick bind is disabled, perform a full descriptor update
                 hdrFullRefresh = true;
                 return pipeline->SyncDescriptors(ctx, constantBuffers.boundConstantBuffers, samplers, textures,
                                                  activeDescriptorSetSampledImages,
                                                  srcStageMask, dstStageMask);
             }
         }()};

         activeDescriptorPipeline = pipeline;

         if (oldPipeline != pipeline)
             // If the pipeline has changed, we need to update the pipeline state
             builder.SetPipeline(pipeline->compiledPipeline.pipeline, vk::PipelineBindPoint::eGraphics);

         if (descUpdateInfo) {
             if (pipeline->compiledPipeline.usesPushDescriptors) {
                 builder.SetDescriptorSetWithPush(descUpdateInfo);
             } else {
                 if (!attachedDescriptorSets)
                     attachedDescriptorSets = std::make_shared<boost::container::static_vector<DescriptorAllocator::ActiveDescriptorSet, DescriptorBatchSize>>();

                 auto newSet{&attachedDescriptorSets->emplace_back(ctx.gpu.descriptor.AllocateSet(descUpdateInfo->descriptorSetLayout))};
                 auto *oldSet{activeDescriptorSet};
                 activeDescriptorSet = newSet;

                 builder.SetDescriptorSetWithUpdate(descUpdateInfo, activeDescriptorSet, oldSet);

                 if (attachedDescriptorSets->size() == DescriptorBatchSize) {
                     ctx.executor.AttachDependency(attachedDescriptorSets);
                     attachedDescriptorSets.reset();
                 }
             }
         }

         CaptureHdrTrace(this, pipeline, descUpdateInfo, hdrFullRefresh, constantBuffers.boundConstantBuffers, activeDescriptorSetSampledImages, activeState.GetColorAttachments(), activeState.GetDepthAttachment(), ctx.executor);

         if (pipeline->UsesRenderArea()) {
             const auto &surfaceClip{clearEngineRegisters.surfaceClip};
             builder.SetRenderArea(
                 *pipeline->compiledPipeline.pipelineLayout,
                 {
                     static_cast<f32>(surfaceClip.horizontal.width),
                     static_cast<f32>(surfaceClip.vertical.height),
                     0.0f,
                     0.0f,
                 });
         }
    }


    namespace {
        struct HdrOffsetRange { u8 bank; u16 first, last; };
        // PR #379: constant-index OpAccessChain in the two fragment SPIR-V
        // programs; CBUF6 has dynamic indices and is NOT included.
        constexpr HdrOffsetRange HdrDraw1[] = {{0,1296,1296}, {2,400,400}, {2,472,472}, {2,1408,1408}, {2,1432,1432}, {3,508,508}, {3,704,764}, {3,1040,1068}, {3,1120,1128}, {3,2064,2068}, {3,2088,2092}, {3,2104,2124}, {3,2160,2164}, {3,2268,2268}, {3,2292,2292}, {3,2512,2520}, {3,2940,2940}, {3,2992,3000}, {3,3128,3132}, {3,3140,3140}, {3,3620,3620}, {4,76,76}, {4,320,324}, {4,380,380}, {4,412,412}, {5,0,0}, {5,8,8}, {5,16,24}, {5,32,32}, {5,48,56}, {5,64,72}, {5,80,88}, {5,96,96}, {5,104,108}, {5,1280,1284}, {5,2048,2048}, {7,112,120}, {7,192,200}, {7,260,260}, {7,268,268}, {7,280,280}, {7,288,300}, {7,308,316}, {7,324,328}};
        constexpr HdrOffsetRange HdrDraw7[] = {{0,1296,1296}, {2,512,512}, {2,1464,1464}, {3,508,508}, {3,704,764}, {3,1040,1068}, {3,1120,1128}, {3,2064,2068}, {3,2088,2092}, {3,2104,2124}, {3,2160,2164}, {3,2280,2280}, {3,2292,2292}, {3,2512,2520}, {3,2940,2940}, {3,2992,3000}, {3,3128,3132}, {3,3140,3140}, {3,3620,3620}, {4,96,104}, {4,112,120}, {4,128,136}, {4,144,152}, {4,320,324}, {4,380,380}, {4,412,412}, {5,0,0}, {5,8,8}, {5,16,24}, {5,32,32}, {5,48,56}, {5,64,72}, {5,80,88}, {5,96,96}, {5,104,108}, {5,1280,1284}, {5,2048,2048}, {7,16,20}, {7,32,36}, {7,48,56}, {7,64,72}, {7,80,88}, {7,96,104}, {7,128,136}, {7,192,200}, {7,208,216}, {7,240,248}, {7,256,264}, {7,288,296}, {7,368,376}, {7,468,476}, {7,520,540}, {7,548,556}, {7,564,576}, {7,584,588}, {7,596,596}, {7,608,608}, {7,616,616}, {7,624,636}, {7,644,656}};

        void CaptureHdrTrace(const Maxwell3D *owner, Pipeline *pipeline, DescriptorUpdateInfo *update, bool full,
                             ConstantBufferSet &buffers, span<TextureView *> sampled,
                             span<TextureView *> targets, TextureView *depth, CommandExecutor &executor) {
            struct ImageRecord {
                vk::DescriptorImageInfo info;
                u32 generation;
                bool full;
            };
            struct Shadow {
                const Maxwell3D *owner{};
                std::map<std::pair<u32,u32>, ImageRecord> images;
                u32 generation{};
                std::array<u32,2> captures{};
            };
            // GPU command execution is serial per thread. Do not combine
            // shadow state from two different Maxwell3D/engine instances.
            static thread_local Shadow shadow;
            if (shadow.owner != owner) { shadow = Shadow{}; shadow.owner = owner; }
            if (full) shadow.images.clear();
            if (update) {
                ++shadow.generation;
                for (const auto &w : update->writes) {
                    if (!w.pImageInfo ||
                        (w.descriptorType != vk::DescriptorType::eCombinedImageSampler &&
                         w.descriptorType != vk::DescriptorType::eStorageImage &&
                         w.descriptorType != vk::DescriptorType::eSampledImage &&
                         w.descriptorType != vk::DescriptorType::eSampler))
                        continue;
                    for (u32 i{}; i < w.descriptorCount; ++i)
                        shadow.images[{w.dstBinding, w.dstArrayElement+i}] =
                            ImageRecord{w.pImageInfo[i], shadow.generation, full};
                }
            }
            const u64 hash{pipeline->sourcePackedState.shaderHashes[5]};
            const int focus{hash == 0xEA18B70958415FF3ULL ? 0 :
                            hash == 0x1B8CB95A440E18C1ULL ? 1 : -1};
            if (focus < 0 || shadow.captures[focus] >= 2) return;
            auto ranges{focus == 0 ? std::span<const HdrOffsetRange>(HdrDraw1)
                                   : std::span<const HdrOffsetRange>(HdrDraw7)};
            std::vector<std::string> cbufLogs;
            cbufLogs.reserve(ranges.size());
            for (auto const &r : ranges) {
                auto &cbuf{buffers[4][r.bank]};
                if (!cbuf.view || static_cast<size_t>(r.last) + 4 > cbuf.view.size) {
                    cbufLogs.push_back(fmt::format("HDR_CBUF hash={:016X} bank={} offsets={}-{} missing=1",
                                                    hash,r.bank,r.first,r.last));
                    continue;
                }
                std::string vals;
                for (u32 offset{r.first}; offset <= r.last; offset += 4)
                    vals += fmt::format("{}{:08X}", vals.empty() ? "" : ",",
                                        cbuf.ReadForShaderCompile<u32>(executor,offset));
                cbufLogs.push_back(fmt::format("HDR_CBUF hash={:016X} bank={} start={} u32={}",
                                                hash,r.bank,r.first,vals));
            }
            auto n{++shadow.captures[focus]};
            LOGI("HDR_FOCUS_BEGIN hash={:016X} occurrence={} mode={} source=vk_descriptor_writes",
                 hash,n,full ? "full" : update ? "partial" : "reused");
            for (auto const &[location,entry] : shadow.images) {
                auto &v{entry.info};
                LOGI("HDR_BIND hash={:016X} binding={} element={} vk_view=0x{:X} vk_sampler=0x{:X} layout={} written_at={} write_type={}",
                     hash,location.first,location.second,
                     std::bit_cast<u64>(static_cast<VkImageView>(v.imageView)),
                     std::bit_cast<u64>(static_cast<VkSampler>(v.sampler)),
                     static_cast<u32>(v.imageLayout),entry.generation,
                     entry.full ? "full" : "partial");
            }
            for (size_t i{};i<sampled.size();++i) {
                auto *view{sampled[i]};
                if (!view) continue;
                LOGI("HDR_SAMPLED hash={:016X} slot={} view={} type={} format={} mip={} levels={} layer={} layers={}",
                     hash,i,fmt::ptr(view),static_cast<u32>(view->type),
                     static_cast<u32>(view->format->vkFormat),view->range.baseMipLevel,
                     view->range.levelCount,view->range.baseArrayLayer,view->range.layerCount);
            }
            for (size_t i{}; i < targets.size(); ++i) {
                auto *view{targets[i]};
                if (view)
                    LOGI("HDR_TARGET hash={:016X} slot={} texture={} format={} mip={} layer={}",
                         hash,i,fmt::ptr(view->texture.get()),
                         static_cast<u32>(view->format->vkFormat),
                         view->range.baseMipLevel,view->range.baseArrayLayer);
            }
            if (depth)
                LOGI("HDR_DEPTH hash={:016X} texture={} format={} mip={} layer={}",
                     hash,fmt::ptr(depth->texture.get()),
                     static_cast<u32>(depth->format->vkFormat),
                     depth->range.baseMipLevel,depth->range.baseArrayLayer);
            for (auto const &line : cbufLogs) LOGI("{}",line);
            LOGI("HDR_CBUF_DYNAMIC hash={:016X} bank=6 dynamic_index=unresolved",hash);
            LOGI("HDR_FOCUS_END hash={:016X} occurrence={}",hash,n);
        }
    }

    void Maxwell3D::LoadConstantBuffer(span<u32> data, u32 offset) {
        constantBuffers.Load(ctx, data, offset);
    }

    void Maxwell3D::BindConstantBuffer(engine::ShaderStage stage, u32 index, bool enable) {
        if (enable)
            constantBuffers.Bind(ctx, stage, index);
        else
            constantBuffers.Unbind(stage, index);
    }

    void Maxwell3D::DisableQuickConstantBufferBind() {
        constantBuffers.DisableQuickBind();
    }

    void Maxwell3D::DrawTexture(const engine::DrawTexture &params,
                                 const engine::WindowOrigin &origin,
                                 const engine::SurfaceClip &clip, size_t colorTargetIndex) {
        ctx.executor.RunWithTextureBackingRetry([&] {
        TRACE_EVENT("gpu", "Maxwell3D::DrawTexture");

        // DRAW_TEXTURE uses its own texture/sampler IDs, not the shaders or
        // descriptors of the previously selected programmable pipeline.
        samplers.Update(ctx, samplerBinding.value == engine::SamplerBinding::Value::ViaHeaderBinding);
        auto *source{textures.GetTexture(ctx, params.textureIndex, Shader::TextureType::Color2D)};
        auto *sampler{samplers.GetSampler(ctx, params.samplerIndex)};
        auto destination{activeState.GetColorRenderTargetForClear(ctx, colorTargetIndex)};
        if (!source || !sampler || !destination)
            return;

        auto *dest{destination.get()};
        if (source == dest || source->texture.get() == dest->texture.get()) {
            // A feedback loop needs explicit Vulkan layout/dependency support.
            LOGW("DRAW_TEXTURE source and destination refer to the same image");
            return;
        }

        constexpr float Fixed12Scale{1.0f / 4096.0f};
        constexpr double Fixed32Scale{1.0 / 4294967296.0};

        const float width{params.dstWidth * Fixed12Scale};
        const float height{params.dstHeight * Fixed12Scale};
        const float x{params.dstX * Fixed12Scale};
        float y{params.dstY * Fixed12Scale};
        if (origin.lowerLeft)
            y = static_cast<float>(clip.vertical.height) - y;
        // Window-origin bit 4 flips triangle/shader Y. DRAW_TEXTURE already
        // supplies window coordinates and only uses the origin mode (bit 0).

        const float srcX{params.srcX * Fixed12Scale};
        const float srcY{params.srcY * Fixed12Scale};
        const float srcWidth{static_cast<float>(params.duDx * Fixed32Scale) * width};
        const float srcHeight{static_cast<float>(params.dvDy * Fixed32Scale) * height};

        // Aliases and TIC base levels can select a mip of a larger backing image.
        // Coordinates are relative to that view, as are Vulkan sampler UVs.
        auto viewDimensions{[](TextureView *view) {
            const auto mip{std::min(view->range.baseMipLevel, 31U)};
            return vk::Extent2D{
                std::max(1U, view->texture->dimensions.width >> mip),
                std::max(1U, view->texture->dimensions.height >> mip)
            };
        }};
        const auto srcDims{viewDimensions(source)};
        const auto dstDims{viewDimensions(dest)};
        float left{std::max({std::min(x, x + width), 0.0f, static_cast<float>(clip.horizontal.x)})};
        float top{std::max({std::min(y, y + height), 0.0f, static_cast<float>(clip.vertical.y)})};
        float right{std::min({std::max(x, x + width), static_cast<float>(dstDims.width),
                             static_cast<float>(clip.horizontal.x) + clip.horizontal.width})};
        float bottom{std::min({std::max(y, y + height), static_cast<float>(dstDims.height),
                              static_cast<float>(clip.vertical.y) + clip.vertical.height})};
        const auto &scissor0{clearEngineRegisters.scissor0};
        if (scissor0.enable) {
            left = std::max(left, static_cast<float>(scissor0.horizontal.xMin));
            right = std::min(right, static_cast<float>(scissor0.horizontal.xMax));
            const float scissorTop{origin.lowerLeft
                ? static_cast<float>(clip.vertical.height) - scissor0.vertical.yMax
                : static_cast<float>(scissor0.vertical.yMin)};
            const float scissorBottom{origin.lowerLeft
                ? static_cast<float>(clip.vertical.height) - scissor0.vertical.yMin
                : static_cast<float>(scissor0.vertical.yMax)};
            top = std::max(top, scissorTop);
            bottom = std::min(bottom, scissorBottom);
        }
        if (!(right > left && bottom > top) || !srcDims.width || !srcDims.height)
            return;

        const auto x0{static_cast<i32>(std::floor(left))};
        const auto y0{static_cast<i32>(std::floor(top))};
        const auto x1{static_cast<i32>(std::ceil(right))};
        const auto y1{static_cast<i32>(std::ceil(bottom))};
        const vk::Rect2D renderArea{
            {x0, y0}, {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}
        };

        // AttachTexture only keeps the backing locked through submission. Retain
        // the sampled view on the fence cycle too, even if its TIC cache is replaced.
        ctx.executor.AttachDependency(source->shared_from_this());
        ctx.executor.AttachDependency(destination);
        ctx.executor.AttachTexture(source);
        ctx.executor.AttachTexture(dest);
        dest->texture->MarkGpuDirty(ctx.executor.usageTracker);

        RefreshRenderCondition();
        ctx.executor.NotifyPipelineChange();
        ctx.executor.AddCheckpoint("Before draw texture");
        const auto renderCondition{activeRenderCondition};
        ctx.gpu.helperShaders.blitHelperShader.Blit(
            ctx.gpu,
            {.width = srcWidth, .height = srcHeight, .x = srcX, .y = srcY},
            {.width = width, .height = height, .x = x, .y = y},
            srcDims, dstDims, false, source, dest,
            [this, renderArea, source, dest, renderCondition](auto &&executionCallback) {
                std::array<TextureView *, 1> sampledImages{source};
                std::array<TextureView *, 1> colorAttachments{dest};
                ctx.executor.AddSubpass(
                    [renderCondition, executionCallback = std::move(executionCallback)]
                    (vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &cycle, GPU &gpu, vk::RenderPass renderPass, u32 subpass) mutable {
                        if (renderCondition.enabled) {
                            auto binding{renderCondition.Resolve(gpu)};
                            commandBuffer.beginConditionalRenderingEXT(vk::ConditionalRenderingBeginInfoEXT{
                                .buffer = binding.buffer, .offset = binding.offset,
                            });
                        }
                        executionCallback(commandBuffer, cycle, gpu, renderPass, subpass);
                        if (renderCondition.enabled)
                            commandBuffer.endConditionalRenderingEXT();
                    }, renderArea, sampledImages, {}, colorAttachments, {},
                    false, vk::PipelineStageFlagBits::eAllGraphics, vk::PipelineStageFlagBits::eAllGraphics,
                    [this](u32 renderPassIndex) {
                        return queries.PrepareDraw(ctx, Queries::CounterType::Occulusion, sampleCounterEnable != 0, renderPassIndex);
                    });
            },
            **sampler, renderArea
        );
        ctx.executor.AddCheckpoint("After draw texture");
    
        });
    }

    void Maxwell3D::FlushQueries() {
        queries.Flush(ctx);
        // Keep the requested predicate across texture-backing retries, but resolve its
        // mappings and GPU preparation again in the new execution.
        renderConditionLhsView.PurgeCaches();
        renderConditionRhsView.PurgeCaches();
        preparedRenderCondition.reset();
        preparedRenderConditionLhs.reset();
        preparedRenderConditionRhs.reset();
    }

    void Maxwell3D::ClearRenderCondition() {
        requestedRenderCondition.reset();
        activeRenderCondition = {};
    }

    bool Maxwell3D::CheckRenderCondition(soc::gm20b::IOVA lhs, std::optional<soc::gm20b::IOVA> rhs, RenderConditionMode mode) {
        ClearRenderCondition();
        if (ctx.gpu.traits.supportsConditionalRendering) {
            // Preparation can publish GPU nodes, so defer it until the draw/clear
            // has resolved every texture that might request a submission and retry.
            requestedRenderCondition = RenderConditionKey{mode, u64{lhs}, rhs ? u64{*rhs} : 0};
            return true;
        }

        // BufferView::Read waits on prior cycles itself, and only submits current
        // work when a GPU-dirty input needs it. Clean CPU predicates never submit.
        const size_t operandSize{mode == RenderConditionMode::NonZero ? sizeof(u32) : sizeof(u64)};
        auto read{[&](soc::gm20b::IOVA address) {
            CachedMappedBufferView mapped{};
            mapped.Update(ctx, u64{address}, operandSize);
            if (!mapped.view || mapped.view.size < operandSize)
                throw exception("Unmapped render-condition operand at 0x{:X}", u64{address});
            auto view{mapped.view};
            std::optional<ContextLock<BufferView>> lock;
            lock.emplace(ctx.executor.tag, view);
            auto buffer{view.GetBuffer()->shared_from_this()};
            u64 value{};
            view.Read(lock->IsFirstUsage(), [&] {
                if (buffer->IsCurrentExecutionGpuDirty()) {
                    lock.reset();
                    ctx.executor.Submit();
                    lock.emplace(ctx.executor.tag, view);
                }
            }, span<u8>{reinterpret_cast<u8 *>(&value), operandSize}, 0);
            return value;
        }};
        const u64 lhsValue{read(lhs)};
        if (mode == RenderConditionMode::NonZero)
            return lhsValue != 0;
        if (!rhs)
            throw exception("Missing render-condition comparison operand");
        const bool equal{lhsValue == read(*rhs)};
        return mode == RenderConditionMode::NotEqual ? !equal : equal;
    }

    void Maxwell3D::RefreshRenderCondition() {
        activeRenderCondition = {};
        if (requestedRenderCondition) {
            const auto key{*requestedRenderCondition};
            if (!SetRenderCondition(soc::gm20b::IOVA{key.lhs},
                                    key.mode == RenderConditionMode::NonZero ? std::nullopt : std::optional{soc::gm20b::IOVA{key.rhs}},
                                    key.mode))
                throw exception("Unable to prepare GPU render condition");
        }
    }

    void Maxwell3D::InvalidateRenderConditionAfterDraw(bool transformFeedbackEnable) {
        if (!preparedRenderCondition)
            return;
        auto mayWrite{[&](const std::optional<RenderConditionInput> &input) {
            return input &&
                ((activeDescriptorPipeline && activeDescriptorPipeline->MayWriteBuffer(input->buffer)) ||
                 (transformFeedbackEnable && activeState.MayWriteTransformFeedbackBuffer(input->buffer)));
        }};
        if (mayWrite(preparedRenderConditionLhs) || mayWrite(preparedRenderConditionRhs))
            preparedRenderCondition.reset();
    }

    bool Maxwell3D::SetRenderCondition(soc::gm20b::IOVA lhs, std::optional<soc::gm20b::IOVA> rhs, RenderConditionMode mode) {
        activeRenderCondition = {};
        if (!ctx.gpu.traits.supportsConditionalRendering)
            return false;

        const RenderConditionKey key{mode, u64{lhs}, rhs ? u64{*rhs} : 0};
        const size_t operandSize{mode == RenderConditionMode::NonZero ? sizeof(u32) : sizeof(u64)};
        renderConditionLhsView.Update(ctx, u64{lhs}, operandSize);
        if (!renderConditionLhsView.view || renderConditionLhsView.view.size < operandSize)
            return false;
        std::optional<RenderConditionInput> rhsInput;
        if (mode != RenderConditionMode::NonZero) {
            if (!rhs)
                return false;
            renderConditionRhsView.Update(ctx, u64{*rhs}, sizeof(u64));
            if (!renderConditionRhsView.view || renderConditionRhsView.view.size < sizeof(u64))
                return false;
        }

        // Resolving the second operand may merge buffers and update both delegates.
        // Capture identities and generations only after both mappings are resolved.
        ctx.executor.AttachBuffer(*renderConditionLhsView);
        renderConditionLhsView.view.GetBuffer()->BlockSequencedCpuBackingWrites();
        const RenderConditionInput lhsInput{
            renderConditionLhsView.view.GetBuffer(), renderConditionLhsView.view.GetOffset(),
            renderConditionLhsView.view.GetBuffer()->GetWriteGeneration()};
        if (mode != RenderConditionMode::NonZero) {
            ctx.executor.AttachBuffer(*renderConditionRhsView);
            renderConditionRhsView.view.GetBuffer()->BlockSequencedCpuBackingWrites();
            rhsInput = RenderConditionInput{
                renderConditionRhsView.view.GetBuffer(), renderConditionRhsView.view.GetOffset(),
                renderConditionRhsView.view.GetBuffer()->GetWriteGeneration()};
        }

        const bool needsPrepare{
            !preparedRenderCondition || !(*preparedRenderCondition == key) ||
            preparedRenderConditionExecutionTag != ctx.executor.executionTag ||
            preparedRenderConditionLhs != lhsInput || preparedRenderConditionRhs != rhsInput
        };
        // Retain both the scratch and its descriptor set through every consuming
        // cycle, including a retry that reuses previously computed contents.
        auto resources{renderConditionResources};
        ctx.executor.AttachDependency(resources);
        if (needsPrepare) {
            ctx.executor.BreakRenderPass();
            BufferView lhsView{renderConditionLhsView.view};
            BufferView rhsView{mode == RenderConditionMode::NonZero ? BufferView{} : renderConditionRhsView.view};
            const bool notEqual{mode == RenderConditionMode::NotEqual};
            const bool nonZero{mode == RenderConditionMode::NonZero};
            ctx.executor.AddOutsideRpCommand(
                [lhsView, rhsView, resources, notEqual, nonZero, operandSize]
                (vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu) mutable {
                    auto lhsBinding{lhsView.GetBinding(gpu)};
                    const vk::Buffer scratchBuffer{resources->scratch.vkBuffer};
                    commandBuffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::eAllCommands,
                        vk::PipelineStageFlagBits::eTransfer,
                        {},
                        vk::MemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eConditionalRenderingReadEXT,
                            .dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
                        }, {}, {});
                    commandBuffer.copyBuffer(lhsBinding.buffer, scratchBuffer, vk::BufferCopy{
                        .srcOffset = lhsBinding.offset, .dstOffset = 0, .size = operandSize,
                    });
                    if (rhsView) {
                        auto rhsBinding{rhsView.GetBinding(gpu)};
                        commandBuffer.copyBuffer(rhsBinding.buffer, scratchBuffer, vk::BufferCopy{
                            .srcOffset = rhsBinding.offset, .dstOffset = sizeof(u64), .size = sizeof(u64),
                        });
                    }
                    commandBuffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eComputeShader,
                        {},
                        vk::MemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
                        }, {}, {});
                    gpu.helperShaders.conditionalCompareHelperShader.Compare(commandBuffer, *resources->descriptorSet, notEqual, nonZero);
                    commandBuffer.pipelineBarrier(
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eConditionalRenderingEXT,
                        {},
                        vk::MemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                            .dstAccessMask = vk::AccessFlagBits::eConditionalRenderingReadEXT,
                        }, {}, {});
                });
            preparedRenderCondition = key;
            preparedRenderConditionExecutionTag = ctx.executor.executionTag;
            preparedRenderConditionLhs = lhsInput;
            preparedRenderConditionRhs = rhsInput;
        }
        activeRenderCondition = RenderConditionBinding{
            {resources->scratch.vkBuffer, sizeof(u32) * 4, sizeof(u32)}, true};
        return true;
    }

    void Maxwell3D::Clear(engine::ClearSurface &clearSurface) {
        ctx.executor.RunWithTextureBackingRetry([&] {
        auto scissor{GetClearScissor()};
        if (scissor.extent.width == 0 || scissor.extent.height == 0)
            return;

        TRACE_EVENT("gpu", "Maxwell3D::Clear");
        // Resolve all requested attachment views before publishing any clear node.
        // If a backing promotion is required, the retry starts from a clean preparation state.
        auto requestedColor{(clearSurface.rEnable || clearSurface.gEnable || clearSurface.bEnable || clearSurface.aEnable)
            ? activeState.GetColorRenderTargetForClear(ctx, clearSurface.mrtSelect) : std::shared_ptr<TextureView>{}};
        auto requestedDepth{(clearSurface.stencilEnable || clearSurface.zEnable)
            ? activeState.GetDepthRenderTargetForClear(ctx) : std::shared_ptr<TextureView>{}};
        RefreshRenderCondition();
        queries.Pause(ctx);
        ctx.executor.AddCheckpoint("Before clear");

        const auto renderCondition{activeRenderCondition};
        auto conditionSubpass{[renderCondition](CommandExecutor::SubpassFunction &&function) -> CommandExecutor::SubpassFunction {
            if (!renderCondition.enabled)
                return std::move(function);

            return [renderCondition, function = std::move(function)]
                   (vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &cycle, GPU &gpu, vk::RenderPass renderPass, u32 subpass) mutable {
                auto conditionBinding{renderCondition.Resolve(gpu)};
                commandBuffer.beginConditionalRenderingEXT(vk::ConditionalRenderingBeginInfoEXT{
                    .buffer = conditionBinding.buffer,
                    .offset = conditionBinding.offset,
                });

                function(commandBuffer, cycle, gpu, renderPass, subpass);
                commandBuffer.endConditionalRenderingEXT();
            };
        }};

        auto needsAttachmentClearCmd{[&](auto &view) {
            return scissor.offset.x != 0 || scissor.offset.y != 0 ||
                scissor.extent != vk::Extent2D{view->texture->dimensions} ||
                view->range.layerCount != 1 || view->range.baseArrayLayer != 0 || clearSurface.rtArrayIndex != 0;
        }};

        // Always use surfaceClip for render area since it's more likely to match the renderArea of draws and avoid an RP break
        const auto &surfaceClip{clearEngineRegisters.surfaceClip};
        vk::Rect2D renderArea{{surfaceClip.horizontal.x, surfaceClip.vertical.y}, {surfaceClip.horizontal.width, surfaceClip.vertical.height}};

        auto clearRects{util::MakeFilledArray<vk::ClearRect, 2>(vk::ClearRect{.rect = scissor, .baseArrayLayer = clearSurface.rtArrayIndex, .layerCount = 1})};
        boost::container::small_vector<vk::ClearAttachment, 2> clearAttachments;

        std::shared_ptr<TextureView> colorView{};
        std::shared_ptr<TextureView> depthStencilView{};

        if (clearSurface.rEnable || clearSurface.gEnable || clearSurface.bEnable || clearSurface.aEnable) {
            if (auto view{requestedColor}) {
                ctx.executor.AttachTexture(&*view);

                bool partialClear{!(clearSurface.rEnable && clearSurface.gEnable && clearSurface.bEnable && clearSurface.aEnable)};
                if (!(view->range.aspectMask & vk::ImageAspectFlagBits::eColor))
                    LOGW("Colour RT used in clear lacks colour aspect"); // TODO: Drop this check after texman rework


                if (partialClear) {
                    ctx.gpu.helperShaders.clearHelperShader.Clear(ctx.gpu, view->range.aspectMask,
                                                                  (clearSurface.rEnable ? vk::ColorComponentFlagBits::eR : vk::ColorComponentFlags{}) |
                                                                  (clearSurface.gEnable ? vk::ColorComponentFlagBits::eG : vk::ColorComponentFlags{}) |
                                                                  (clearSurface.bEnable ? vk::ColorComponentFlagBits::eB : vk::ColorComponentFlags{}) |
                                                                  (clearSurface.aEnable ? vk::ColorComponentFlagBits::eA : vk::ColorComponentFlags{}),
                                                                  {clearEngineRegisters.colorClearValue}, &*view, [=](auto &&executionCallback) {
                        auto dst{view.get()};
                        ctx.executor.AddSubpass(conditionSubpass(std::move(executionCallback)), renderArea, {}, {}, span<TextureView *>{dst}, nullptr);
                    });
                    ctx.executor.NotifyPipelineChange();
                } else if (renderCondition.enabled || needsAttachmentClearCmd(view)) {
                    clearAttachments.push_back({.aspectMask = view->range.aspectMask, .clearValue = {clearEngineRegisters.colorClearValue}});
                    colorView = view;
                } else {
                    ctx.executor.AddClearColorSubpass(&*view, clearEngineRegisters.colorClearValue);
                }
            }
        }

        if (clearSurface.stencilEnable || clearSurface.zEnable) {
            if (auto view{requestedDepth}) {
                ctx.executor.AttachTexture(&*view);

                bool viewHasDepth{view->range.aspectMask & vk::ImageAspectFlagBits::eDepth}, viewHasStencil{view->range.aspectMask & vk::ImageAspectFlagBits::eStencil};
                vk::ImageAspectFlags clearAspectMask{(clearSurface.zEnable ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlags{}) |
                                                     (clearSurface.stencilEnable ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlags{})};
                clearAspectMask &= view->range.aspectMask;

                vk::ClearDepthStencilValue clearValue{
                    .depth = clearEngineRegisters.depthClearValue,
                    .stencil = clearEngineRegisters.stencilClearValue
                };

                if (!clearAspectMask) {
                    LOGW("Depth stencil RT used in clear lacks depth or stencil aspects"); // TODO: Drop this check after texman rework
                    return;
                }

                if (renderCondition.enabled || needsAttachmentClearCmd(view) || (clearAspectMask != view->range.aspectMask)) { // Conditional/load-op clears cannot express the guest predicate, and subpass clears write all aspects.
                    clearAttachments.push_back({.aspectMask = clearAspectMask, .clearValue = clearValue});
                    depthStencilView = view;
                } else {
                    ctx.executor.AddClearDepthStencilSubpass(&*view, clearValue);
                }
            }
        }

        if (!clearAttachments.empty()) {
            std::array<TextureView *, 1> colorAttachments{colorView ? &*colorView : nullptr};
            ctx.executor.AddSubpass(conditionSubpass([clearAttachments, clearRects](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32) {
                commandBuffer.clearAttachments(clearAttachments, span(clearRects).first(clearAttachments.size()));
            }), renderArea, {}, {}, colorView ? colorAttachments : span<TextureView *>{}, depthStencilView ? &*depthStencilView : nullptr);
        }

        ctx.executor.AddCheckpoint("After clear");
    
        });
    }

    void Maxwell3D::Draw(engine::DrawTopology topology, bool transformFeedbackEnable, bool indexed, u32 count, u32 first, u32 instanceCount, u32 vertexOffset, u32 firstInstance) {
        ctx.executor.RunWithTextureBackingRetry([&] {
        TRACE_EVENT("gpu", "Draw", "indexed", indexed, "count", count, "instanceCount", instanceCount);

        StateUpdateBuilder builder{*ctx.executor.allocator};
        vk::PipelineStageFlags srcStageMask{}, dstStageMask{};

        PrepareDraw(builder, topology, indexed, false, first, count, srcStageMask, dstStageMask);

        if (directState.inputAssembly.NeedsQuadConversion()) {
            count = conversion::quads::GetIndexCount(count);
            first = 0;

            if (!indexed) {
                // Use an index buffer to emulate quad lists with a triangle list input topology
                vk::DeviceSize offset{UpdateQuadConversionBuffer(count, first)};
                builder.SetIndexBuffer(BufferBinding{quadConversionBuffer->vkBuffer, offset}, vk::IndexType::eUint32);
                indexed = true;
            }
        }

        RefreshRenderCondition();
        auto stateUpdater{builder.Build()};

        /**
         * @brief Struct that can be linearly allocated, holding all state for the draw to avoid a dynamic allocation with lambda captures
         */
        struct DrawParams {
            StateUpdater stateUpdater;
            RenderConditionBinding renderCondition;
            u32 count;
            u32 first;
            u32 instanceCount;
            u32 vertexOffset;
            u32 firstInstance;
            bool indexed;
            bool transformFeedbackEnable;
        };
        auto *drawParams{ctx.executor.allocator->EmplaceUntracked<DrawParams>(DrawParams{stateUpdater,
                                                                                         activeRenderCondition,
                                                                                         count, first, instanceCount, vertexOffset, firstInstance, indexed,
                                                                                         ctx.gpu.traits.supportsTransformFeedback ? transformFeedbackEnable : false})};

        vk::Rect2D scissor{GetDrawScissor()};


        constantBuffers.ResetQuickBind();
        ctx.executor.AddCheckpoint("Before draw");
        ctx.executor.AddSubpass([drawParams](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu, vk::RenderPass, u32) {
            drawParams->stateUpdater.RecordAll(gpu, commandBuffer);

            if (drawParams->renderCondition.enabled) {
                auto conditionBinding{drawParams->renderCondition.Resolve(gpu)};
                commandBuffer.beginConditionalRenderingEXT(vk::ConditionalRenderingBeginInfoEXT{
                    .buffer = conditionBinding.buffer,
                    .offset = conditionBinding.offset,
                });
            }

            if (drawParams->transformFeedbackEnable)
                commandBuffer.beginTransformFeedbackEXT(0, {}, {});

            if (drawParams->indexed)
                commandBuffer.drawIndexed(drawParams->count, drawParams->instanceCount, drawParams->first, static_cast<i32>(drawParams->vertexOffset), drawParams->firstInstance);
            else
                commandBuffer.draw(drawParams->count, drawParams->instanceCount, drawParams->first, drawParams->firstInstance);

            if (drawParams->transformFeedbackEnable)
                commandBuffer.endTransformFeedbackEXT(0, {}, {});

            if (drawParams->renderCondition.enabled)
                commandBuffer.endConditionalRenderingEXT();
        }, scissor, activeDescriptorSetSampledImages, {}, activeState.GetColorAttachments(), activeState.GetDepthAttachment(), !ctx.gpu.traits.quirks.relaxedRenderPassCompatibility, srcStageMask, dstStageMask,
           [this](u32 renderPassIndex) {
               return queries.PrepareDraw(ctx, Queries::CounterType::Occulusion, sampleCounterEnable != 0, renderPassIndex);
           });
        InvalidateRenderConditionAfterDraw(transformFeedbackEnable && ctx.gpu.traits.supportsTransformFeedback);
        ctx.executor.AddCheckpoint("After draw");
    
        });
    }

    void Maxwell3D::DrawIndirect(engine::DrawTopology topology, bool transformFeedbackEnable, bool indexed, span<u8> indirectBuffer, u32 count, u32 stride) {
        ctx.executor.RunWithTextureBackingRetry([&] {
        if (!count)
            return;

        TRACE_EVENT("gpu", "Indirect Draw", "buffer", reinterpret_cast<uintptr_t>(indirectBuffer.data()));

        StateUpdateBuilder builder{*ctx.executor.allocator};
        vk::PipelineStageFlags srcStageMask{}, dstStageMask{};

        PrepareDraw(builder, topology, indexed, true, 0, 0, srcStageMask, dstStageMask);

        if (directState.inputAssembly.NeedsQuadConversion())
            throw exception("Quad conversion is not supported for indirect draws!");

        if (indirectBufferView)
            indirectBufferView = indirectBufferView.GetBuffer()->TryGetView(indirectBuffer);
        if (!indirectBufferView)
            indirectBufferView = ctx.gpu.buffer.FindOrCreate(indirectBuffer, ctx.executor.tag, [this](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
            });

        indirectBufferView.GetBuffer()->BlockSequencedCpuBackingWrites();

        RefreshRenderCondition();
        auto stateUpdater{builder.Build()};

        /**
         * @brief Struct that can be linearly allocated, holding all state for the draw to avoid a dynamic allocation with lambda captures
         */
        struct DrawParams {
            StateUpdater stateUpdater;
            BufferView indirectBuffer;
            RenderConditionBinding renderCondition;
            u32 count;
            u32 stride;
            bool indexed;
            bool transformFeedbackEnable;
        };
        auto *drawParams{ctx.executor.allocator->EmplaceUntracked<DrawParams>(DrawParams{stateUpdater,
                                                                                         indirectBufferView,
                                                                                         activeRenderCondition,
                                                                                         count, stride, indexed,
                                                                                         ctx.gpu.traits.supportsTransformFeedback ? transformFeedbackEnable : false})};

        auto scissor{GetDrawScissor()};
        constantBuffers.ResetQuickBind();

        ctx.executor.AddCheckpoint("Before indirect draw");
        ctx.executor.AddSubpass([drawParams](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu, vk::RenderPass, u32) {
            drawParams->stateUpdater.RecordAll(gpu, commandBuffer);

            if (drawParams->renderCondition.enabled) {
                auto conditionBinding{drawParams->renderCondition.Resolve(gpu)};
                commandBuffer.beginConditionalRenderingEXT(vk::ConditionalRenderingBeginInfoEXT{
                    .buffer = conditionBinding.buffer,
                    .offset = conditionBinding.offset,
                });
            }

            if (drawParams->transformFeedbackEnable)
                commandBuffer.beginTransformFeedbackEXT(0, {}, {});

            auto indirectBinding{drawParams->indirectBuffer.GetBinding(gpu)};
            if (drawParams->indexed)
                commandBuffer.drawIndexedIndirect(indirectBinding.buffer, indirectBinding.offset, drawParams->count, drawParams->stride);
            else
                commandBuffer.drawIndirect(indirectBinding.buffer,  indirectBinding.offset, drawParams->count, drawParams->stride);

            if (drawParams->transformFeedbackEnable)
                commandBuffer.endTransformFeedbackEXT(0, {}, {});

            if (drawParams->renderCondition.enabled)
                commandBuffer.endConditionalRenderingEXT();
        }, scissor, activeDescriptorSetSampledImages, {}, activeState.GetColorAttachments(), activeState.GetDepthAttachment(), !ctx.gpu.traits.quirks.relaxedRenderPassCompatibility, srcStageMask, dstStageMask,
           [this](u32 renderPassIndex) {
               return queries.PrepareDraw(ctx, Queries::CounterType::Occulusion, sampleCounterEnable != 0, renderPassIndex);
           });
        InvalidateRenderConditionAfterDraw(transformFeedbackEnable && ctx.gpu.traits.supportsTransformFeedback);
        ctx.executor.AddCheckpoint("After indirect draw");
    
        });
    }

    void Maxwell3D::Query(soc::gm20b::IOVA address, engine::SemaphoreInfo::CounterType type, std::optional<u64> timestamp) {
        if (type != engine::SemaphoreInfo::CounterType::SamplesPassed) {
            LOGE("Unsupported query type: {}", static_cast<u32>(type));
            return;
        }

        queries.Query(ctx, address, Queries::CounterType::Occulusion, timestamp);
    }

    void Maxwell3D::ResetCounter(engine::ClearReportValue::Type type) {
        if (type != engine::ClearReportValue::Type::ZPassPixelCount) {
            LOGD("Unsupported query type: {}", static_cast<u32>(type));
            return;
        }

        queries.ResetCounter(ctx, Queries::CounterType::Occulusion);
    }

}
