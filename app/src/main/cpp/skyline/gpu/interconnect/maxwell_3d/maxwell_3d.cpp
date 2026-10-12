// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cmath>
#include <optional>
#include <bit>
#include <gpu/interconnect/command_executor.h>
#include <gpu/interconnect/conversion/quads.h>
#include <gpu/interconnect/common/state_updater.h>
#include <soc/gm20b/channel.h>
#include "common/utils.h"
#include "maxwell_3d.h"
#include "common.h"
#include <gpu/diagnostic/re5_scene_capture.h>
#include <gpu/diagnostic/re5_scalar_probe.h>
#include <gpu/diagnostic/re5_texture_trace.h>

namespace skyline::gpu::interconnect::maxwell3d {
    // Phase 2: filter by structural GPU properties observed in RE5 gameplay.
    // No Title ID checks, Vulkan state changes, forced refreshes or render-pass
    // splits. The hashes only prioritize diagnostic logs; they do not affect draws.
    template <typename ActiveStateType>
    static bool HasDiagnosticSceneAttachments(ActiveStateType &activeState) {
        auto &capture{diagnostic::Re5SceneCapture()};
        if (!capture.active.load(std::memory_order_acquire))
            return false;
        auto *depth{activeState.GetDepthAttachment()};
        const bool hasDepth{depth && depth->texture};
        const bool depthAspect{hasDepth && (depth->texture->format->vkAspect & vk::ImageAspectFlagBits::eDepth)};
        const u32 depthWidth{hasDepth ? depth->texture->dimensions.width : 0};
        const u32 depthHeight{hasDepth ? depth->texture->dimensions.height : 0};
        bool sceneTarget{};
        bool colorSeen{};
        for (auto *view : activeState.GetColorAttachments()) {
            if (!view || !view->texture) continue;
            colorSeen = true;
            const bool colorAspect{bool(view->texture->format->vkAspect & vk::ImageAspectFlagBits::eColor)};
            capture.RecordSceneGeometry({depthWidth, depthHeight, static_cast<u32>(depthAspect),
                view->texture->dimensions.width, view->texture->dimensions.height, static_cast<u32>(colorAspect)});
            if (depthAspect && colorAspect && diagnostic::SceneExtentMatches(view->texture->dimensions.width,
                    view->texture->dimensions.height, depthWidth, depthHeight)) {
                sceneTarget = true;
                break;
            }
        }
        if (!colorSeen)
            capture.RecordSceneGeometry({depthWidth, depthHeight, static_cast<u32>(depthAspect), 0, 0, 0});
        return sceneTarget;
    }

    template <typename ActiveStateType>
    static std::optional<u32> SampleRe5SceneDraw(ActiveStateType &activeState) {
        auto &capture{diagnostic::Re5SceneCapture()};
        if (!HasDiagnosticSceneAttachments(activeState))
            return std::nullopt;
        bool priority{};
        if (auto *pipeline{activeState.GetPipeline()}) {
            for (const auto hash : pipeline->sourcePackedState.shaderHashes) {
                if (hash == 0xF3805B306B15F89AULL ||
                    hash == 0xB141A1171435D55EULL ||
                    hash == 0x8E1C37D501C478E1ULL ||
                    hash == 0x07C66B919FC55A45ULL) {
                    priority = true;
                    break;
                }
            }
        }
        return capture.SampleSceneDraw(priority);
    }

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
          directState{activeState.directState},
          queries{gpu} {
        ctx.executor.AddFlushCallback([this] {
            diagnosticUniformBuffers = {};
            diagnosticImageBindings.clear();
            if (attachedDescriptorSets) {
                ctx.executor.AttachDependency(attachedDescriptorSets);
                attachedDescriptorSets = nullptr;
                activeDescriptorSet = nullptr;
            }

            activeState.MarkAllDirty();
            constantBuffers.MarkAllDirty();
            samplers.MarkAllDirty();
            textures.MarkAllDirty();
            quadConversionBufferAttached = false;
            constantBuffers.DisableQuickBind();
            queries.PurgeCaches(ctx);
        });

        ctx.executor.AddPipelineChangeCallback([this] {
            diagnosticUniformBuffers = {};
            diagnosticImageBindings.clear();
            activeState.MarkAllDirty();
            activeDescriptorSet = nullptr;
        });
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
     TRACE_EVENT("gpu", "DrawWithInlineIndex", "count", count, "instanceCount", instanceCount);

       StateUpdateBuilder builder{*ctx.executor.allocator};
       vk::PipelineStageFlags srcStageMask{}, dstStageMask{};

    PrepareDraw(builder, topology, true, false, 0, count, srcStageMask, dstStageMask);
    auto scalarProbe{PrepareDiagnosticScalarProbe("inline", count, instanceCount)};
    auto bindingProbe{PrepareDiagnosticBindingProbe("inline", count, instanceCount)};

    inlineIndexBuffer = std::make_shared<memory::Buffer>(ctx.gpu.memory.AllocateBuffer(util::AlignUp(indexData.size_bytes(), PAGE_SIZE)));
    std::memcpy(inlineIndexBuffer->cast<u8>().data(), indexData.data(), indexData.size_bytes());
    ctx.executor.AttachDependency(inlineIndexBuffer);

    builder.SetIndexBuffer(BufferBinding{inlineIndexBuffer->vkBuffer, 0}, ConvertInlineIndexSize(indexSize));

    auto stateUpdater{builder.Build()};

    struct DrawParams {
        StateUpdater stateUpdater;
        u32 count;
        u32 instanceCount;
        bool transformFeedbackEnable;
    };
    auto *drawParams{ctx.executor.allocator->EmplaceUntracked<DrawParams>(DrawParams{stateUpdater,
        count, instanceCount, ctx.gpu.traits.supportsTransformFeedback ? transformFeedbackEnable : false})};

    vk::Rect2D scissor{GetDrawScissor()};
    constantBuffers.ResetQuickBind();

    ctx.executor.AddCheckpoint("Before inline index draw");
    ctx.executor.AddSubpass([drawParams](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu, vk::RenderPass, u32) {
        drawParams->stateUpdater.RecordAll(gpu, commandBuffer);
        if (drawParams->transformFeedbackEnable)
            commandBuffer.beginTransformFeedbackEXT(0, {}, {});
        commandBuffer.drawIndexed(drawParams->count, drawParams->instanceCount, 0, 0, 0);
        if (drawParams->transformFeedbackEnable)
            commandBuffer.endTransformFeedbackEXT(0, {}, {});
    }, scissor, activeDescriptorSetSampledImages, {}, activeState.GetColorAttachments(), activeState.GetDepthAttachment(),
         !ctx.gpu.traits.quirks.relaxedRenderPassCompatibility, srcStageMask, dstStageMask, std::move(scalarProbe), std::move(bindingProbe));

        if (auto re5SceneOrdinal{SampleRe5SceneDraw(activeState)}) {
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
            // Track the depth target alongside color RTs to distinguish
            // shadow/depth failures from shader output and late composition.
            if (auto *depthView{activeState.GetDepthAttachment()}; depthView && depthView->texture) {
                targets += fmt::format("depth=0x{:X}:{}x{}:{},",
                    reinterpret_cast<uintptr_t>(depthView->texture.get()),
                    depthView->texture->dimensions.width, depthView->texture->dimensions.height,
                    vk::to_string(depthView->texture->format->vkFormat));
            } else {
                targets += "depth=null,";
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
            diagnostic::Re5SceneCapture().Record(fmt::format(
                "SCENE_DRAW ordinal={} kind=inline indexed=1 count={} instances={} scissor={},{},{}x{} shaders={} targets={} blend={} sampled={}",
                *re5SceneOrdinal, count, instanceCount, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                scissor.extent.height, shaderHashes, targets, blend, sampledViews));
        }
          ctx.executor.AddCheckpoint("After inline index draw");
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
             if (((oldPipeline == pipeline) || (oldPipeline && oldPipeline->CheckBindingMatch(pipeline))) && constantBuffers.quickBindEnabled) {
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

         if (oldPipeline != pipeline)
             // If the pipeline has changed, we need to update the pipeline state
             builder.SetPipeline(pipeline->compiledPipeline.pipeline, vk::PipelineBindPoint::eGraphics);

         if (descUpdateInfo) {
             // Full updates replace provenance. Quick-bind copies keep only
             // slots whose binding/element is demonstrably copied unchanged.
             diagnostic::UpdateScalarProvenance(diagnosticUniformBuffers,
                 descUpdateInfo->diagnosticUniformBuffers, descUpdateInfo->copies);
             diagnostic::UpdateImageProvenance(diagnosticImageBindings,
                 descUpdateInfo->diagnosticImageBindings, descUpdateInfo->copies, 64);
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
    }

    std::shared_ptr<DiagnosticBindingProbe> Maxwell3D::PrepareDiagnosticBindingProbe(const char *kind, u32 count, u32 instances) {
        auto &capture{diagnostic::Re5SceneCapture()};
        if (!capture.active.load(std::memory_order_acquire)) return {};
        auto *pipeline{activeState.GetPipeline()};
        auto *depth{activeState.GetDepthAttachment()};
        if (!pipeline || !HasDiagnosticSceneAttachments(activeState)) return {};
        const auto hash{pipeline->sourcePackedState.shaderHashes[static_cast<size_t>(engine::Pipeline::Shader::Type::Pixel)]};
        const u32 group{hash == 0xF3805B306B15F89AULL ? 0U : hash == 0xB141A1171435D55EULL ? 1U :
            hash == 0x8E1C37D501C478E1ULL ? 2U : hash == 0x07C66B919FC55A45ULL ? 3U : 4U};
        const auto interval{capture.presentIndex.load(std::memory_order_acquire)};
        auto ticket{capture.ReserveBindingProbe(interval, group)};
        if (!ticket) return {};
        auto probe{std::make_shared<DiagnosticBindingProbe>()};
        probe->directory = std::move(ticket->directory);
        probe->index = ticket->index;
        probe->metadata = fmt::format("draw_binding_probe={} approx_present={} kind={} count={} instances={} descriptor_set=0x{:X} descriptor_mode={} retained_slots={} pipeline_sampled_count={} slot_limit=64 capture_slot_limit=16\nshaders=",
            probe->index, interval, kind, count, instances,
            activeDescriptorSet ? reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(**activeDescriptorSet)) : 0,
            ctx.gpu.traits.supportsPushDescriptors ? "push" : "allocated_set", diagnosticImageBindings.size(),
            pipeline->GetTotalSampledImageCount());
        for (const auto shaderHash : pipeline->sourcePackedState.shaderHashes)
            probe->metadata += fmt::format("{:016X},", shaderHash);
        probe->metadata += '\n';
        probe->metadata += fmt::format("depth_texture_id={} dimensions={}x{}\n",
            depth->texture->diagnosticIdentity, depth->texture->dimensions.width,
            depth->texture->dimensions.height);
        for (auto *view : activeState.GetColorAttachments())
            if (view && view->texture)
                probe->metadata += fmt::format("target_texture_id={} object=0x{:X}\n",
                    view->texture->diagnosticIdentity, reinterpret_cast<uintptr_t>(view->texture.get()));
        size_t captured{};
        for (const auto &image : diagnosticImageBindings) {
            if (captured++ >= 16) break;
            probe->images.push_back(image);
            probe->metadata += fmt::format("BINDING binding={} array_element={} stage={} handle=0x{:08X} primary_cbuf={} primary_cbuf_offset={} aggregate_src_stages={} aggregate_dst_stages={}\n{}\n",
                image.binding, image.element, image.stage, image.rawHandle, image.cbufIndex, image.cbufOffset,
                vk::to_string(image.aggregateSrcStages), vk::to_string(image.aggregateDstStages),
                diagnostic::DescribeTextureView(image.view));
            if (image.view && image.view->texture) {
                auto &texture{*image.view->texture};
                const auto ranges{texture.guest ? diagnostic::TextureMappings(*texture.guest) : std::vector<diagnostic::MemoryRange>{}};
                const auto related{diagnostic::Re5BlitJournal().PinRelated(probe->directory, texture.diagnosticIdentity, ranges)};
                probe->metadata += "related_blit_ids=";
                for (const auto id : related) probe->metadata += fmt::format("{},", id);
                probe->metadata += " relation=same_texture_or_physical_overlap_not_proven_pixel_producer\n";
                // A single targeted probe for the 128x128 cube observed at FS binding 21
                // in the supplied paired captures. Read-only: no descriptor refresh or sync.
                if (image.stage == 4 && image.binding == 21 &&
                    image.view->type == vk::ImageViewType::eCube &&
                    texture.dimensions.width == 128 && texture.dimensions.height == 128) {
                    constexpr u32 LodCbufIndex{12};
                    constexpr u32 LodOffset{4};
                    probe->metadata += fmt::format("CUBEMAP_128_BIND21 {}\n",
                        samplers.DiagnosticLodSummary(image.rawHandle >> 20, image.rawHandle & ((1U << 20) - 1U)));
                    auto &lodView{constantBuffers.boundConstantBuffers[4][LodCbufIndex].view};
                    if (lodView && diagnostic::ScalarRangeValid(lodView.size, LodOffset)) {
                        ContextLock lock{ctx.executor.tag, lodView};
                        const auto base{lodView.GetOffset()};
                        const auto lod{lodView.GetBuffer()->PeekDiagnosticGuestScalar(base + LodOffset)};
                        probe->metadata += fmt::format(
                            "CUBEMAP_128_BIND21 lod_cbuf=12 lod_offset=0x4 guest_status={} "
                            "gpu_dirty={} direct={} shadow={} view_offset={} view_size={}\n",
                            lod.bits ? "unsynchronized_guest_mirror" : "guest_mirror_unavailable",
                            lod.gpuDirty, lod.direct, lod.directShadow, base, lodView.size);
                        if (lod.bits)
                            probe->metadata += fmt::format("CUBEMAP_128_BIND21 lod_bits=0x{:08X} lod_float={} "
                                "note=guest_observation_not_shader_execution\n", *lod.bits, std::bit_cast<float>(*lod.bits));
                    } else {
                        probe->metadata += "CUBEMAP_128_BIND21 lod_status=unbound_or_out_of_range\n";
                    }
                }
            }
            if (probe->metadata.size() > 48 * 1024) {
                probe->metadata += "metadata_budget_exhausted\n";
                break;
            }
        }
        probe->metadata += "note=states_at_draw_preparation; actual_descriptor_fields_at_command_recording; history_is_bounded_and_may_be_incomplete\n";
        return probe;
    }

    std::shared_ptr<DiagnosticScalarProbe> Maxwell3D::PrepareDiagnosticScalarProbe(const char *kind, u32 count, u32 instances) {
        auto &capture{diagnostic::Re5SceneCapture()};
        if (!capture.active.load(std::memory_order_acquire))
            return {};
        auto *pipeline{activeState.GetPipeline()};
        if (!pipeline)
            return {};
        const auto hash{pipeline->sourcePackedState.shaderHashes[static_cast<size_t>(engine::Pipeline::Shader::Type::Pixel)]};
        const u32 target{hash == 0xF3805B306B15F89AULL ? 0U : 1U};
        if (hash != 0xF3805B306B15F89AULL && hash != 0xB141A1171435D55EULL)
            return {};
        auto *depth{activeState.GetDepthAttachment()};
        if (!HasDiagnosticSceneAttachments(activeState))
            return {};
        const auto interval{capture.presentIndex.load(std::memory_order_acquire)};
        if (!capture.SampleScalarDraw(interval, target))
            return {};
        auto ticket{capture.ReserveScalarProbe(interval)};
        if (!ticket)
            return {};
        auto probe{std::make_shared<DiagnosticScalarProbe>()};
        probe->descriptor = diagnosticUniformBuffers[target];
        probe->byteOffset = target == 0 ? 0x1C0 : 0;
        probe->index = ticket->index;
        probe->directory = std::move(ticket->directory);
        probe->metadata = fmt::format(
            "probe={} approx_present={} kind={} count={} instances={} fs={:016X} stage=4 cbuf={} byte_offset={} "
            "binding={} array_element={} descriptor_set=0x{:X} descriptor_mode={} source={} "
            "guest_time=draw_preparation gpu_time=natural_render_pass_end depth_texture=0x{:X}\n",
            probe->index, interval, kind, count, instances, hash, target + 3, probe->byteOffset,
            probe->descriptor.binding, probe->descriptor.element,
            activeDescriptorSet ? reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(**activeDescriptorSet)) : 0,
            ctx.gpu.traits.supportsPushDescriptors ? "push" : "allocated_set",
            !probe->descriptor.info ? "unavailable" : probe->descriptor.immutableMegaBuffer ? "megabuffer" : "buffer_view",
            reinterpret_cast<uintptr_t>(depth->texture.get()));
        probe->metadata += "shaders=";
        for (const auto shaderHash : pipeline->sourcePackedState.shaderHashes)
            probe->metadata += fmt::format("{:016X},", shaderHash);
        probe->metadata += '\n';
        for (auto *view : activeState.GetColorAttachments())
            if (view && view->texture)
                probe->metadata += fmt::format("color_texture=0x{:X} dimensions={}x{} format={}\n",
                    reinterpret_cast<uintptr_t>(view->texture.get()), view->texture->dimensions.width,
                    view->texture->dimensions.height, vk::to_string(view->texture->format->vkFormat));
        auto &view{constantBuffers.boundConstantBuffers[4][target + 3].view};
        if (view && diagnostic::ScalarRangeValid(view.size, probe->byteOffset)) {
            ContextLock lock{ctx.executor.tag, view};
            const auto base{view.GetOffset()};
            auto guest{view.GetBuffer()->PeekDiagnosticGuestScalar(base + probe->byteOffset)};
            probe->metadata += fmt::format(
                "guest_status={} guest_map=0x{:X} guest_view_offset={} guest_view_size={} direct={} gpu_dirty={} direct_shadow={}\n",
                guest.bits ? "mirror_observed_without_synchronization" : "mirror_unavailable", guest.guestMap,
                base, view.size, guest.direct, guest.gpuDirty, guest.directShadow);
            if (guest.bits)
                probe->metadata += fmt::format("guest_bits=0x{:08X} guest_float={}\n", *guest.bits, std::bit_cast<float>(*guest.bits));
        } else {
            probe->metadata += "guest_status=unbound_or_out_of_range\n";
        }
        probe->metadata += "note=guest_mirror_may_be_stale_or_differ_from_direct_shadow; gpu_read_is_postpass_not_shader_execution_proof\n";
        return probe;
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
        TRACE_EVENT("gpu", "Maxwell3D::DrawTexture");

        // DRAW_TEXTURE uses its own texture/sampler IDs, not the shaders or
        // descriptors of the previously selected programmable pipeline.
        samplers.Update(ctx, samplerBinding.value == engine::SamplerBinding::Value::ViaHeaderBinding);
        auto *source{textures.GetTexture(ctx, params.textureIndex, Shader::TextureType::Color2D)};
        auto *sampler{samplers.GetSampler(ctx, params.samplerIndex)};
        auto destination{activeState.GetColorRenderTargetForClear(ctx, colorTargetIndex)};
        if (diagnostic::Re5SceneCapture().active.load(std::memory_order_acquire)) {
            diagnostic::Re5SceneCapture().Record(fmt::format(
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

        ctx.executor.AddCheckpoint("Before draw texture");
        ctx.gpu.helperShaders.blitHelperShader.Blit(
            ctx.gpu,
            {.width = srcWidth, .height = srcHeight, .x = srcX, .y = srcY},
            {.width = width, .height = height, .x = x, .y = y},
            srcDims, dstDims, false, source, dest,
            [this, renderArea, source, dest](auto &&executionCallback) {
                std::array<TextureView *, 1> sampledImages{source};
                std::array<TextureView *, 1> colorAttachments{dest};
                ctx.executor.AddSubpass(std::move(executionCallback), renderArea,
                                        sampledImages, {}, colorAttachments, {},
                                        false, vk::PipelineStageFlagBits::eAllGraphics,
                                        vk::PipelineStageFlagBits::eAllGraphics);
            },
            **sampler, renderArea
        );
        ctx.executor.AddCheckpoint("After draw texture");
        ctx.executor.NotifyPipelineChange();
    }

    void Maxwell3D::Clear(engine::ClearSurface &clearSurface) {
        auto scissor{GetClearScissor()};
        if (scissor.extent.width == 0 || scissor.extent.height == 0)
            return;

        TRACE_EVENT("gpu", "Maxwell3D::Clear");
        ctx.executor.AddCheckpoint("Before clear");

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
            if (auto view{activeState.GetColorRenderTargetForClear(ctx, clearSurface.mrtSelect)}) {
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
                        ctx.executor.AddSubpass(std::move(executionCallback), renderArea, {}, {}, span<TextureView *>{dst}, nullptr);
                    });
                    ctx.executor.NotifyPipelineChange();
                } else if (needsAttachmentClearCmd(view)) {
                    clearAttachments.push_back({.aspectMask = view->range.aspectMask, .clearValue = {clearEngineRegisters.colorClearValue}});
                    colorView = view;
                } else {
                    ctx.executor.AddClearColorSubpass(&*view, clearEngineRegisters.colorClearValue);
                }
            }
        }

        if (clearSurface.stencilEnable || clearSurface.zEnable) {
            if (auto view{activeState.GetDepthRenderTargetForClear(ctx)}) {
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

                if (needsAttachmentClearCmd(view) || (clearAspectMask != view->range.aspectMask)) { // Subpass clears write to all aspects of the texture, so we can't use them when only one component is enabled
                    clearAttachments.push_back({.aspectMask = clearAspectMask, .clearValue = clearValue});
                    depthStencilView = view;
                } else {
                    ctx.executor.AddClearDepthStencilSubpass(&*view, clearValue);
                }
            }
        }

        if (!clearAttachments.empty()) {
            std::array<TextureView *, 1> colorAttachments{colorView ? &*colorView : nullptr};
            ctx.executor.AddSubpass([clearAttachments, clearRects](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32) {
                commandBuffer.clearAttachments(clearAttachments, span(clearRects).first(clearAttachments.size()));
            }, renderArea, {}, {}, colorView ? colorAttachments : span<TextureView *>{}, depthStencilView ? &*depthStencilView : nullptr);
        }

        ctx.executor.AddCheckpoint("After clear");
    }

    void Maxwell3D::Draw(engine::DrawTopology topology, bool transformFeedbackEnable, bool indexed, u32 count, u32 first, u32 instanceCount, u32 vertexOffset, u32 firstInstance) {
        TRACE_EVENT("gpu", "Draw", "indexed", indexed, "count", count, "instanceCount", instanceCount);

        StateUpdateBuilder builder{*ctx.executor.allocator};
        vk::PipelineStageFlags srcStageMask{}, dstStageMask{};

        PrepareDraw(builder, topology, indexed, false, first, count, srcStageMask, dstStageMask);
        auto scalarProbe{PrepareDiagnosticScalarProbe("direct", count, instanceCount)};
        auto bindingProbe{PrepareDiagnosticBindingProbe("direct", count, instanceCount)};

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

        auto stateUpdater{builder.Build()};

        /**
         * @brief Struct that can be linearly allocated, holding all state for the draw to avoid a dynamic allocation with lambda captures
         */
        struct DrawParams {
            StateUpdater stateUpdater;
            u32 count;
            u32 first;
            u32 instanceCount;
            u32 vertexOffset;
            u32 firstInstance;
            bool indexed;
            bool transformFeedbackEnable;
        };
        auto *drawParams{ctx.executor.allocator->EmplaceUntracked<DrawParams>(DrawParams{stateUpdater,
                                                                                         count, first, instanceCount, vertexOffset, firstInstance, indexed,
                                                                                         ctx.gpu.traits.supportsTransformFeedback ? transformFeedbackEnable : false})};

        vk::Rect2D scissor{GetDrawScissor()};


        constantBuffers.ResetQuickBind();
        ctx.executor.AddCheckpoint("Before draw");
        ctx.executor.AddSubpass([drawParams](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu, vk::RenderPass, u32) {
            drawParams->stateUpdater.RecordAll(gpu, commandBuffer);

            if (drawParams->transformFeedbackEnable)
                commandBuffer.beginTransformFeedbackEXT(0, {}, {});

            if (drawParams->indexed)
                commandBuffer.drawIndexed(drawParams->count, drawParams->instanceCount, drawParams->first, static_cast<i32>(drawParams->vertexOffset), drawParams->firstInstance);
            else
                commandBuffer.draw(drawParams->count, drawParams->instanceCount, drawParams->first, drawParams->firstInstance);

            if (drawParams->transformFeedbackEnable)
                commandBuffer.endTransformFeedbackEXT(0, {}, {});
        }, scissor, activeDescriptorSetSampledImages, {}, activeState.GetColorAttachments(), activeState.GetDepthAttachment(), !ctx.gpu.traits.quirks.relaxedRenderPassCompatibility, srcStageMask, dstStageMask, std::move(scalarProbe), std::move(bindingProbe));

        if (auto re5SceneOrdinal{SampleRe5SceneDraw(activeState)}) {
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
            // Track the depth target alongside color RTs to distinguish
            // shadow/depth failures from shader output and late composition.
            if (auto *depthView{activeState.GetDepthAttachment()}; depthView && depthView->texture) {
                targets += fmt::format("depth=0x{:X}:{}x{}:{},",
                    reinterpret_cast<uintptr_t>(depthView->texture.get()),
                    depthView->texture->dimensions.width, depthView->texture->dimensions.height,
                    vk::to_string(depthView->texture->format->vkFormat));
            } else {
                targets += "depth=null,";
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
            diagnostic::Re5SceneCapture().Record(fmt::format(
                "SCENE_DRAW ordinal={} kind=direct indexed={} count={} instances={} scissor={},{},{}x{} shaders={} targets={} blend={} sampled={}",
                *re5SceneOrdinal, indexed, count, instanceCount, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                scissor.extent.height, shaderHashes, targets, blend, sampledViews));
        }
        ctx.executor.AddCheckpoint("After draw");
    }

    void Maxwell3D::DrawIndirect(engine::DrawTopology topology, bool transformFeedbackEnable, bool indexed, span<u8> indirectBuffer, u32 count, u32 stride) {
        if (!count)
            return;

        TRACE_EVENT("gpu", "Indirect Draw", "buffer", reinterpret_cast<uintptr_t>(indirectBuffer.data()));

        StateUpdateBuilder builder{*ctx.executor.allocator};
        vk::PipelineStageFlags srcStageMask{}, dstStageMask{};

        PrepareDraw(builder, topology, indexed, true, 0, 0, srcStageMask, dstStageMask);
        auto scalarProbe{PrepareDiagnosticScalarProbe("indirect", count, 0)};
        auto bindingProbe{PrepareDiagnosticBindingProbe("indirect", count, 0)};

        if (directState.inputAssembly.NeedsQuadConversion())
            throw exception("Quad conversion is not supported for indirect draws!");

        if (indirectBufferView)
            indirectBufferView = indirectBufferView.GetBuffer()->TryGetView(indirectBuffer);
        if (!indirectBufferView)
            indirectBufferView = ctx.gpu.buffer.FindOrCreate(indirectBuffer, ctx.executor.tag, [this](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
            });

        indirectBufferView.GetBuffer()->BlockSequencedCpuBackingWrites();

        auto stateUpdater{builder.Build()};

        /**
         * @brief Struct that can be linearly allocated, holding all state for the draw to avoid a dynamic allocation with lambda captures
         */
        struct DrawParams {
            StateUpdater stateUpdater;
            BufferView indirectBuffer;
            u32 count;
            u32 stride;
            bool indexed;
            bool transformFeedbackEnable;
        };
        auto *drawParams{ctx.executor.allocator->EmplaceUntracked<DrawParams>(DrawParams{stateUpdater,
                                                                                         indirectBufferView,
                                                                                         count, stride, indexed,
                                                                                         ctx.gpu.traits.supportsTransformFeedback ? transformFeedbackEnable : false})};

        auto scissor{GetDrawScissor()};
        constantBuffers.ResetQuickBind();

        ctx.executor.AddCheckpoint("Before indirect draw");
        ctx.executor.AddSubpass([drawParams](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu, vk::RenderPass, u32) {
            drawParams->stateUpdater.RecordAll(gpu, commandBuffer);

            if (drawParams->transformFeedbackEnable)
                commandBuffer.beginTransformFeedbackEXT(0, {}, {});

            auto indirectBinding{drawParams->indirectBuffer.GetBinding(gpu)};
            if (drawParams->indexed)
                commandBuffer.drawIndexedIndirect(indirectBinding.buffer, indirectBinding.offset, drawParams->count, drawParams->stride);
            else
                commandBuffer.drawIndirect(indirectBinding.buffer,  indirectBinding.offset, drawParams->count, drawParams->stride);

            if (drawParams->transformFeedbackEnable)
                commandBuffer.endTransformFeedbackEXT(0, {}, {});
        }, scissor, activeDescriptorSetSampledImages, {}, activeState.GetColorAttachments(), activeState.GetDepthAttachment(), !ctx.gpu.traits.quirks.relaxedRenderPassCompatibility, srcStageMask, dstStageMask, std::move(scalarProbe), std::move(bindingProbe));

        if (auto re5SceneOrdinal{SampleRe5SceneDraw(activeState)}) {
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
            // Track the depth target alongside color RTs to distinguish
            // shadow/depth failures from shader output and late composition.
            if (auto *depthView{activeState.GetDepthAttachment()}; depthView && depthView->texture) {
                targets += fmt::format("depth=0x{:X}:{}x{}:{},",
                    reinterpret_cast<uintptr_t>(depthView->texture.get()),
                    depthView->texture->dimensions.width, depthView->texture->dimensions.height,
                    vk::to_string(depthView->texture->format->vkFormat));
            } else {
                targets += "depth=null,";
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
            diagnostic::Re5SceneCapture().Record(fmt::format(
                "SCENE_DRAW ordinal={} kind=indirect indexed={} count={} instances=0 scissor={},{},{}x{} shaders={} targets={} blend={} sampled={}",
                *re5SceneOrdinal, indexed, count, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                scissor.extent.height, shaderHashes, targets, blend, sampledViews));
        }
        ctx.executor.AddCheckpoint("After indirect draw");
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

    bool Maxwell3D::QueryPresentAtAddress(soc::gm20b::IOVA address) {
        return queries.QueryPresentAtAddress(address);
    }
}
