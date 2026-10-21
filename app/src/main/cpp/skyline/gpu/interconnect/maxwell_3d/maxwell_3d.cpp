// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cmath>
#include <gpu/interconnect/command_executor.h>
#include <gpu/interconnect/conversion/quads.h>
#include <gpu/interconnect/common/state_updater.h>
#include <soc/gm20b/channel.h>
#include "common/utils.h"
#include "maxwell_3d.h"
#include "common.h"
#include <gpu/diagnostic/ue4_frame_capture.h>

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

        if (diagnostic::Ue4Capture().active.load(std::memory_order_acquire)) {
            std::string shaderHashes;
            if (auto *pipeline{activeState.GetPipeline()}) {
                for (const auto hash : pipeline->sourcePackedState.shaderHashes)
                    shaderHashes += fmt::format("{:016X},", hash);
            }
            std::string targets;
            for (auto *target : activeState.GetColorAttachments()) {
                if (target && target->texture) {
                    targets += fmt::format("0x{:X}:{}x{}:{},",
                        reinterpret_cast<uintptr_t>(target->texture.get()),
                        target->texture->dimensions.width,
                        target->texture->dimensions.height,
                        vk::to_string(target->texture->format->vkFormat));
                } else {
                    targets += "null,";
                }
            }
            std::string blend;
            if (auto *pipeline{activeState.GetPipeline()}) {
                const auto packed{pipeline->sourcePackedState.GetAttachmentBlendState(0)};
                blend = fmt::format("mask=0x{:X},enabled={}",
                    static_cast<u32>(packed.colorWriteMask), static_cast<bool>(packed.blendEnable));
            }
            std::string sampledViews;
            for (size_t i{}; i < activeDescriptorSetSampledImages.size() && i < 8; ++i) {
                auto *view{activeDescriptorSetSampledImages[i]};
                if (view && view->texture)
                    sampledViews += fmt::format("[{}]=0x{:X}:{}x{}:{},", i,
                        reinterpret_cast<uintptr_t>(view->texture.get()),
                        view->texture->dimensions.width, view->texture->dimensions.height,
                        vk::to_string(view->texture->format->vkFormat));
            }
            diagnostic::Ue4Capture().Record(fmt::format(
                "DRAW kind=inline indexed=1 count={} instances={} scissor={},{},{}x{} shaders={} targets={} blend={} sampled={}",
                count, instanceCount, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                scissor.extent.height, shaderHashes, targets, blend, sampledViews));
            if (auto *pipeline{activeState.GetPipeline()})
                ctx.executor.MarkDiagnosticUe4Output(
                    activeDescriptorSetSampledImages, activeState.GetColorAttachments(),
                    !activeDescriptorSetSampledImages.empty());
        }
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


         auto *descUpdateInfo{[&]() -> DescriptorUpdateInfo * {
             if (activeDescriptorPipeline &&
                 ((activeDescriptorPipeline == pipeline) || activeDescriptorPipeline->CheckBindingMatch(pipeline)) &&
                 constantBuffers.quickBindEnabled) {
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
             if (ctx.gpu.traits.supportsPushDescriptors) {
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
        if (diagnostic::Ue4Capture().active.load(std::memory_order_acquire)) {
            diagnostic::Ue4Capture().Record(fmt::format(
                "DRAW_TEXTURE texture_index={} sampler_index={} target={} source_present={} sampler_present={} destination_present={} dst={},{} {}x{}",
                params.textureIndex, params.samplerIndex, colorTargetIndex,
                source != nullptr, sampler != nullptr, static_cast<bool>(destination),
                params.dstX, params.dstY, params.dstWidth, params.dstHeight));
        }
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

        [[maybe_unused]] std::optional<u32> hdrProbe{};
        [[maybe_unused]] bool conversionProbe{};
        if (diagnostic::Ue4Capture().active.load(std::memory_order_acquire)) {
            auto &capture{diagnostic::Ue4Capture()};
            auto *pipeline{activeState.GetPipeline()};
            bool conversionShader{};
            if (pipeline) {
                for (auto hash : pipeline->sourcePackedState.shaderHashes)
                    if (hash == 0x9CDBA917AFDC0187ULL)
                        conversionShader = true;
            }

            // This fragment shader consumed an HDR R11G11B10 image at
            // sampled binding 4 in capture_001 and wrote BGRA8. Match
            // the observed shader identity, never an application Title ID.
            if (conversionShader && capture.ClaimConversion()) {
                conversionProbe = true;
                capture.Record(fmt::format(
                    "CONVERSION_FOCUS shader=9CDBA917AFDC0187 submission={} executor=0x{:X} sampled_count={}",
                    ctx.executor.submissionNumber, reinterpret_cast<uintptr_t>(&ctx.executor),
                    activeDescriptorSetSampledImages.size()));

                for (size_t index{}; index < activeDescriptorSetSampledImages.size(); ++index) {
                    auto *view{activeDescriptorSetSampledImages[index]};
                    if (!view || !view->texture) {
                        capture.Record(fmt::format("FOCUS_DESCRIPTOR index={} null=1", index));
                        continue;
                    }
                    const auto &texture{view->texture};
                    capture.Record(fmt::format(
                        "FOCUS_DESCRIPTOR index={} texture=0x{:X} view_type={} base_mip={} levels={} base_layer={} layers={} "
                        "image={}x{} format={} layout={}",
                        index, reinterpret_cast<uintptr_t>(texture.get()),
                        vk::to_string(view->type), view->range.baseMipLevel,
                        view->range.levelCount, view->range.baseArrayLayer, view->range.layerCount,
                        texture->dimensions.width, texture->dimensions.height,
                        vk::to_string(texture->format->vkFormat), vk::to_string(texture->layout)));
                    // Capture the actual HDR source and small exposure/LUT
                    // inputs, not all potentially huge sampled textures.
                    const bool hdr{texture->format->vkFormat == vk::Format::eB10G11R11UfloatPack32};
                    const bool small{texture->dimensions.width <= 32 && texture->dimensions.height <= 32};
                    if (hdr || small)
                        ctx.executor.QueueUe4FocusedImage(view,
                            hdr ? fmt::format("tonemap_hdr_input_{}", index)
                                : fmt::format("tonemap_aux_input_{}", index), 0);
                }

                // Log CBUF bindings without attempting CPU reads of potentially
                // GPU-dirty data; that could itself change guest execution.
                for (size_t stage{}; stage < constantBuffers.boundConstantBuffers.size(); ++stage) {
                    const auto &stageBuffers{constantBuffers.boundConstantBuffers[stage]};
                    for (size_t idx{}; idx < stageBuffers.size(); ++idx) {
                        const auto &buffer{stageBuffers[idx]};
                        if (buffer.view.size)
                            capture.Record(fmt::format("FOCUS_CBUF stage={} index={} view_size={}",
                                stage, idx, buffer.view.size));
                    }
                }
            }

            // Probe early/mid/late individual draws writing R11G11B10 HDR
            // in the first two approximate presentation intervals.
            for (auto *view : activeState.GetColorAttachments()) {
                if (!view || !view->texture ||
                    view->texture->format->vkFormat != vk::Format::eB10G11R11UfloatPack32)
                    continue;
                hdrProbe = capture.ClaimHdrDraw();
                if (hdrProbe) {
                    // Correlate each bracketing pair with exact shader state,
                    // fully resolved sampled image views, and only descriptor-
                    // referenced CBUF slots. Do not read GPU-dirty CBUF bytes.
                    auto hashes{std::string{}};
                    if (pipeline) {
                        for (const auto hash : pipeline->sourcePackedState.shaderHashes)
                            hashes += fmt::format("{:016X},", hash);
                    }
                    capture.Record(fmt::format(
                        "HDR_DRAW_FOCUS ordinal={} interval={} executor=0x{:X} submission={} "
                        "count={} instances={} indexed={} scissor={},{},{}x{} hashes={} "
                        "target=0x{:X} target_view={}x{} format={}",
                        *hdrProbe, capture.presentIndex.load(std::memory_order_relaxed),
                        reinterpret_cast<uintptr_t>(&ctx.executor), ctx.executor.submissionNumber,
                        count, instanceCount, indexed, scissor.offset.x, scissor.offset.y,
                        scissor.extent.width, scissor.extent.height, hashes,
                        reinterpret_cast<uintptr_t>(view->texture.get()),
                        view->texture->dimensions.width, view->texture->dimensions.height,
                        vk::to_string(view->format->vkFormat)));

                    const auto maxViews{std::min<size_t>(activeDescriptorSetSampledImages.size(), 24)};
                    for (size_t idx{}; idx < maxViews; ++idx) {
                        auto *sample{activeDescriptorSetSampledImages[idx]};
                        if (!sample || !sample->texture) {
                            capture.Record(fmt::format("HDR_DRAW_IMAGE ordinal={} index={} null=1", *hdrProbe, idx));
                            continue;
                        }
                        capture.Record(fmt::format(
                            "HDR_DRAW_IMAGE ordinal={} index={} texture=0x{:X} view_type={} "
                            "mip={} mip_levels={} layer={} layers={} image={}x{} "
                            "view_format={} backing_format={} layout={}",
                            *hdrProbe, idx, reinterpret_cast<uintptr_t>(sample->texture.get()),
                            vk::to_string(sample->type), sample->range.baseMipLevel,
                            sample->range.levelCount, sample->range.baseArrayLayer,
                            sample->range.layerCount, sample->texture->dimensions.width,
                            sample->texture->dimensions.height,
                            vk::to_string(sample->format->vkFormat),
                            vk::to_string(sample->texture->format->vkFormat),
                            vk::to_string(sample->texture->layout)));
                    }
                    if (activeDescriptorSetSampledImages.size() > maxViews)
                        capture.Record(fmt::format("HDR_DRAW_IMAGE_TRUNCATED ordinal={} total={} recorded={}",
                            *hdrProbe, activeDescriptorSetSampledImages.size(), maxViews));

                    if (pipeline) {
                        const auto &info{pipeline->GetDiagnosticDescriptorInfo()};
                        for (size_t stageIdx{}; stageIdx < info.stages.size(); ++stageIdx) {
                            const auto &stageInfo{info.stages[stageIdx]};
                            const auto &slots{constantBuffers.boundConstantBuffers[stageIdx]};
                            for (const auto &uniformDesc : stageInfo.uniformBufferDescs) {
                                for (size_t arrayIdx{}; arrayIdx < uniformDesc.count; ++arrayIdx) {
                                    const auto slot{static_cast<size_t>(uniformDesc.index) + arrayIdx};
                                    if (slot >= slots.size())
                                        continue;
                                    const auto usedBytes{slot < stageInfo.constantBufferUsedSizes.size()
                                        ? stageInfo.constantBufferUsedSizes[slot] : 0};
                                    capture.Record(fmt::format(
                                        "HDR_DRAW_CBUF ordinal={} stage={} index={} "
                                        "bound_bytes={} shader_used_bytes={} source=uniform_descriptor",
                                        *hdrProbe, stageIdx, slot, slots[slot].view.size, usedBytes));
                                }
                            }
                            for (const auto &imageDesc : stageInfo.combinedImageSamplerDescs) {
                                capture.Record(fmt::format(
                                    "HDR_DRAW_TEXTURE_DESC ordinal={} stage={} "
                                    "cbuf={} offset={} count={} type={}",
                                    *hdrProbe, stageIdx, imageDesc.cbuf_index,
                                    imageDesc.cbuf_offset, imageDesc.count,
                                    static_cast<u32>(imageDesc.type)));
                            }
                        }
                    }
                    ctx.executor.QueueUe4FocusedImage(view,
                        fmt::format("hdr_before_{}", *hdrProbe), *hdrProbe);
                }
                break;
            }
        }

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

        if (diagnostic::Ue4Capture().active.load(std::memory_order_acquire)) {
            std::string shaderHashes;
            if (auto *pipeline{activeState.GetPipeline()}) {
                for (const auto hash : pipeline->sourcePackedState.shaderHashes)
                    shaderHashes += fmt::format("{:016X},", hash);
            }
            std::string targets;
            for (auto *target : activeState.GetColorAttachments()) {
                if (target && target->texture) {
                    targets += fmt::format("0x{:X}:{}x{}:{},",
                        reinterpret_cast<uintptr_t>(target->texture.get()),
                        target->texture->dimensions.width,
                        target->texture->dimensions.height,
                        vk::to_string(target->texture->format->vkFormat));
                } else {
                    targets += "null,";
                }
            }
            std::string blend;
            if (auto *pipeline{activeState.GetPipeline()}) {
                const auto packed{pipeline->sourcePackedState.GetAttachmentBlendState(0)};
                blend = fmt::format("mask=0x{:X},enabled={}",
                    static_cast<u32>(packed.colorWriteMask), static_cast<bool>(packed.blendEnable));
            }
            std::string sampledViews;
            for (size_t i{}; i < activeDescriptorSetSampledImages.size() && i < 8; ++i) {
                auto *view{activeDescriptorSetSampledImages[i]};
                if (view && view->texture)
                    sampledViews += fmt::format("[{}]=0x{:X}:{}x{}:{},", i,
                        reinterpret_cast<uintptr_t>(view->texture.get()),
                        view->texture->dimensions.width, view->texture->dimensions.height,
                        vk::to_string(view->texture->format->vkFormat));
            }
            diagnostic::Ue4Capture().Record(fmt::format(
                "DRAW kind=direct indexed={} count={} instances={} scissor={},{},{}x{} shaders={} targets={} blend={} sampled={}",
                indexed, count, instanceCount, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                scissor.extent.height, shaderHashes, targets, blend, sampledViews));
            if (auto *pipeline{activeState.GetPipeline()})
                ctx.executor.MarkDiagnosticUe4Output(
                    activeDescriptorSetSampledImages, activeState.GetColorAttachments(),
                    !activeDescriptorSetSampledImages.empty());
            if (hdrProbe) {
                for (auto *target : activeState.GetColorAttachments()) {
                    if (target && target->texture &&
                        target->texture->format->vkFormat == vk::Format::eB10G11R11UfloatPack32) {
                        ctx.executor.QueueUe4FocusedImage(target,
                            fmt::format("hdr_after_{}", *hdrProbe), *hdrProbe);
                        break;
                    }
                }
            }
            if (conversionProbe) {
                for (auto *target : activeState.GetColorAttachments()) {
                    if (target && target->texture) {
                        ctx.executor.QueueUe4FocusedImage(target, "tonemap_bgra_output", 0);
                        break;
                    }
                }
            }
        }
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

        if (diagnostic::Ue4Capture().active.load(std::memory_order_acquire)) {
            std::string shaderHashes;
            if (auto *pipeline{activeState.GetPipeline()}) {
                for (const auto hash : pipeline->sourcePackedState.shaderHashes)
                    shaderHashes += fmt::format("{:016X},", hash);
            }
            std::string targets;
            for (auto *target : activeState.GetColorAttachments()) {
                if (target && target->texture) {
                    targets += fmt::format("0x{:X}:{}x{}:{},",
                        reinterpret_cast<uintptr_t>(target->texture.get()),
                        target->texture->dimensions.width,
                        target->texture->dimensions.height,
                        vk::to_string(target->texture->format->vkFormat));
                } else {
                    targets += "null,";
                }
            }
            std::string blend;
            if (auto *pipeline{activeState.GetPipeline()}) {
                const auto packed{pipeline->sourcePackedState.GetAttachmentBlendState(0)};
                blend = fmt::format("mask=0x{:X},enabled={}",
                    static_cast<u32>(packed.colorWriteMask), static_cast<bool>(packed.blendEnable));
            }
            std::string sampledViews;
            for (size_t i{}; i < activeDescriptorSetSampledImages.size() && i < 8; ++i) {
                auto *view{activeDescriptorSetSampledImages[i]};
                if (view && view->texture)
                    sampledViews += fmt::format("[{}]=0x{:X}:{}x{}:{},", i,
                        reinterpret_cast<uintptr_t>(view->texture.get()),
                        view->texture->dimensions.width, view->texture->dimensions.height,
                        vk::to_string(view->texture->format->vkFormat));
            }
            diagnostic::Ue4Capture().Record(fmt::format(
                "DRAW kind=indirect indexed={} count={} instances=0 scissor={},{},{}x{} shaders={} targets={} blend={} sampled={}",
                indexed, count, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                scissor.extent.height, shaderHashes, targets, blend, sampledViews));
            if (auto *pipeline{activeState.GetPipeline()})
                ctx.executor.MarkDiagnosticUe4Output(
                    activeDescriptorSetSampledImages, activeState.GetColorAttachments(),
                    !activeDescriptorSetSampledImages.empty());
        }
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
