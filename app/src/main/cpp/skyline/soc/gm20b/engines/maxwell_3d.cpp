// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2018-2020 fincs (https://github.com/devkitPro/deko3d)

#include <boost/preprocessor/repeat.hpp>
#include <gpu/interconnect/command_executor.h>
#include <gpu/diagnostic_copy_tracker.h>
#include <gpu/interconnect/common/tic.h>
#include <soc/gm20b/channel.h>
#include <atomic>
#include <limits>
#include <soc.h>
#include "maxwell/types.h"
#include "maxwell_3d.h"

namespace skyline::soc::gm20b::engine::maxwell3d {
    #define REGTYPE(state) gpu::interconnect::maxwell3d::state::EngineRegisters

    static gpu::interconnect::maxwell3d::PipelineState::EngineRegisters MakePipelineStateRegisters(const Maxwell3D::Registers &registers) {
        return {
            .pipelineStageRegisters = util::MergeInto<REGTYPE(PipelineStageState), type::PipelineCount>(*registers.pipelines, *registers.programRegion),
            .colorRenderTargetsRegisters = util::MergeInto<REGTYPE(ColorRenderTargetState), type::ColorTargetCount>(*registers.colorTargets, *registers.surfaceClip),
            .depthRenderTargetRegisters = {*registers.ztSize, *registers.ztOffset, *registers.ztFormat, *registers.ztBlockSize, *registers.ztArrayPitch, *registers.ztSelect, *registers.ztLayer, *registers.surfaceClip},
            .vertexInputRegisters = {*registers.vertexStreams, *registers.vertexStreamInstance, *registers.vertexAttributes},
            .inputAssemblyRegisters = {*registers.primitiveRestartEnable},
            .tessellationRegisters = {*registers.patchSize, *registers.tessellationParameters},
            .rasterizationRegisters = {*registers.rasterEnable, *registers.frontPolygonMode, *registers.backPolygonMode, *registers.oglCullEnable, *registers.oglCullFace, *registers.windowOrigin, *registers.oglFrontFace, *registers.viewportClipControl, *registers.polyOffset, *registers.provokingVertex, *registers.pointSize, registers.viewports[0], registers.viewportClips[0]},
            .depthStencilRegisters = {*registers.depthTestEnable, *registers.depthWriteEnable, *registers.depthFunc, *registers.depthBoundsTestEnable, *registers.stencilTestEnable, *registers.twoSidedStencilTestEnable, *registers.stencilOps, *registers.stencilBack, *registers.alphaTestEnable, *registers.alphaFunc, *registers.alphaRef},
            .colorBlendRegisters = {*registers.logicOp, *registers.singleCtWriteControl, *registers.ctWrites, *registers.blendStatePerTargetEnable, *registers.blendPerTargets, *registers.blend},
            .transformFeedbackRegisters = {*registers.streamOutputEnable, *registers.streamOutControls, *registers.streamOutLayoutSelect},
            .globalShaderConfigRegisters = {*registers.postVtgShaderAttributeSkipMask, *registers.bindlessTexture, *registers.apiMandatedEarlyZEnable, *registers.viewportScaleOffsetEnable},
            .ctSelect = *registers.ctSelect
        };
    }

    static gpu::interconnect::maxwell3d::ActiveState::EngineRegisters MakeActiveStateRegisters(const Maxwell3D::Registers &registers) {
        return {
            .pipelineRegisters = MakePipelineStateRegisters(registers),
            .vertexBuffersRegisters = util::MergeInto<REGTYPE(VertexBufferState), type::VertexStreamCount>(*registers.vertexStreams, *registers.vertexStreamLimits),
            .indexBufferRegisters = {*registers.indexBuffer},
            .transformFeedbackBuffersRegisters = util::MergeInto<REGTYPE(TransformFeedbackBufferState), type::StreamOutBufferCount>(*registers.streamOutBuffers, *registers.streamOutputEnable),
            .viewportsRegisters = util::MergeInto<REGTYPE(ViewportState), type::ViewportCount>(registers.viewports[0], registers.viewportClips[0], *registers.viewports, *registers.viewportClips, *registers.windowOrigin, *registers.viewportScaleOffsetEnable, *registers.surfaceClip),
            .scissorsRegisters = util::MergeInto<REGTYPE(ScissorState), type::ViewportCount>(*registers.scissors),
            .lineWidthRegisters = {*registers.lineWidth, *registers.lineWidthAliased, *registers.aliasedLineWidthEnable},
            .depthBiasRegisters = {*registers.depthBias, *registers.depthBiasClamp, *registers.slopeScaleDepthBias},
            .blendConstantsRegisters = {*registers.blendConsts},
            .depthBoundsRegisters = {*registers.depthBoundsMin, *registers.depthBoundsMax},
            .stencilValuesRegisters = {*registers.stencilValues, *registers.backStencilValues, *registers.twoSidedStencilTestEnable},
        };
    }

    static gpu::interconnect::maxwell3d::Maxwell3D::EngineRegisterBundle MakeEngineRegisters(const Maxwell3D::Registers &registers) {
        return {
            .activeStateRegisters = MakeActiveStateRegisters(registers),
            .clearRegisters = {registers.scissors[0], registers.viewportClips[0], *registers.clearRect, *registers.colorClearValue, *registers.zClearValue, *registers.stencilClearValue, *registers.surfaceClip, *registers.clearSurfaceControl},
            .constantBufferSelectorRegisters = {*registers.constantBufferSelector},
            .samplerPoolRegisters = {*registers.texSamplerPool, *registers.texHeaderPool},
            .samplerBinding = *registers.samplerBinding,
            .texturePoolRegisters = {*registers.texHeaderPool}
        };
    }
    #undef REGTYPE

    type::DrawTopology Maxwell3D::ApplyTopologyOverride(type::DrawTopology beginMethodTopology) {
        return registers.primitiveTopologyControl->override == type::PrimitiveTopologyControl::Override::UseTopologyInBeginMethods ?
               beginMethodTopology : type::ConvertPrimitiveTopologyToDrawTopology(*registers.primitiveTopology);
    }

    bool Maxwell3D::CheckRenderEnable() {
        if (registers.renderEnableOverride->mode == Registers::RenderEnableOverride::Mode::AlwaysRender)
            return true;
        else if (registers.renderEnableOverride->mode == Registers::RenderEnableOverride::Mode::NeverRender)
            return false;



        switch (registers.renderEnable->mode) {
            case Registers::RenderEnable::Mode::True:
                return true;
            case Registers::RenderEnable::Mode::False:
                return false;
            case Registers::RenderEnable::Mode::Conditional:
                // TODO: Use indirect draws to emulate conditional rendering with queries, for now just ignore such cases as they would decrease performance anyway by forcing a CPU sync
                if (interconnect.QueryPresentAtAddress(u64{registers.renderEnable->offset}))
                    return true;

                return channelCtx.asCtx->gmmu.Read<u32>(registers.renderEnable->offset) != 0;
            case Registers::RenderEnable::Mode::RenderIfEqual:
                // TODO: See above
                if (interconnect.QueryPresentAtAddress(u64{registers.renderEnable->offset}) ||
                    interconnect.QueryPresentAtAddress(u64{registers.renderEnable->offset + 16}))
                    return true;

                return channelCtx.asCtx->gmmu.Read<u32>(registers.renderEnable->offset) ==
                    channelCtx.asCtx->gmmu.Read<u32>(registers.renderEnable->offset + 16);
            case Registers::RenderEnable::Mode::RenderIfNotEqual:
                // TODO: See above
                if (interconnect.QueryPresentAtAddress(u64{registers.renderEnable->offset}) ||
                    interconnect.QueryPresentAtAddress(u64{registers.renderEnable->offset + 16}))
                    return true;

                return channelCtx.asCtx->gmmu.Read<u32>(registers.renderEnable->offset) !=
                    channelCtx.asCtx->gmmu.Read<u32>(registers.renderEnable->offset + 16);
        }

        return true;
    }


    Maxwell3D::Maxwell3D(const DeviceState &state, ChannelContext &channelCtx, MacroState &macroState)
        : MacroEngineBase{macroState},
          syncpoints{state.soc->host1x.syncpoints},
          i2m{state, channelCtx},
          dirtyManager{registers},
          interconnect{*state.gpu, channelCtx, state.process->trap, state.process->memory, dirtyManager, MakeEngineRegisters(registers)},
          channelCtx{channelCtx} {
        channelCtx.executor.AddFlushCallback([this]() { FlushEngineState(); });
        InitializeRegisters();
        LOGI("MINIRD_DRAW_TEXTURE_DIAG version={} trigger_method=0x42B channel=0x{:X} "
             "trigger_baseline={} present_candidate_baseline={} unknown_extent_baseline={}",
             gpu::diagnostic::DrawTextureProbeVersion, reinterpret_cast<uintptr_t>(&channelCtx),
             gpu::diagnostic::DrawTextureTriggers().load(std::memory_order_relaxed),
             gpu::diagnostic::DrawTexturePresentCandidates().load(std::memory_order_relaxed),
             gpu::diagnostic::DrawTextureUnknownExtents().load(std::memory_order_relaxed));
    }

    void Maxwell3D::TraceDrawTexture() {
        namespace diagnostic = gpu::diagnostic;
        const auto event{diagnostic::DrawTextureTriggers().fetch_add(1, std::memory_order_relaxed) + 1};
        const auto sequence{++drawTextureDiagnosticSequence};
        const size_t targetIndex{registers.ctSelect->count ? (*registers.ctSelect)[0] : 0};
        const auto &target{registers.colorTargets[targetIndex]};
        const u64 destination{target.offset};

        // Calculate the parent allocation extent, not a written rectangle.
        // No TextureManager lookup, image creation, flush or pixel readback.
        // An unsupported format only makes this diagnostic extent unavailable;
        // it does not change the existing (unimplemented) method's behavior.
        size_t destinationSize{1};
        bool extentKnown{};
        if (target.format != type::ColorTarget::Format::Disabled && target.width && target.height && target.thirdDimension) {
            try {
                if (static_cast<u32>(target.format) > 0xFF)
                    throw exception("RT format exceeds its 8-bit encoding");
                gpu::interconnect::maxwell3d::PackedPipelineState packed{};
                packed.SetColorRenderTargetFormat(targetIndex, target.format);
                gpu::GuestTexture guest{};
                guest.format = packed.GetColorRenderTargetFormat(targetIndex);
                if (!guest.format)
                    throw exception("Empty RT format in diagnostic extent");
                const bool array{target.memory.thirdDimensionControl == type::TargetMemory::ThirdDimensionControl::ThirdDimensionDefinesArraySize};
                guest.layerCount = array ? static_cast<u32>(target.thirdDimension) : 1;
                const u32 depth{array ? 1U : static_cast<u32>(target.thirdDimension)};
                if (target.memory.layout == type::TargetMemory::Layout::Pitch) {
                    guest.dimensions = {target.width / guest.format->bpb, target.height, depth};
                    guest.tileConfig = {.mode = gpu::texture::TileMode::Pitch, .pitch = target.width};
                } else {
                    guest.dimensions = {target.width, target.height, depth};
                    guest.tileConfig = {.mode = gpu::texture::TileMode::Block,
                                        .blockHeight = target.memory.BlockHeight(),
                                        .blockDepth = target.memory.BlockDepth()};
                }
                guest.layerStride = (target.layerOffset > 1 || guest.layerCount > 1) ? target.ArrayPitch() : 0;
                if (guest.dimensions.width && (guest.tileConfig.mode == gpu::texture::TileMode::Pitch ||
                    (guest.tileConfig.blockHeight && guest.tileConfig.blockDepth))) {
                    // CalculateLayerSize returns u32. Bound its intermediate
                    // product before invoking it, including block padding.
                    if (!guest.layerStride) {
                        const size_t rowBytes{guest.tileConfig.mode == gpu::texture::TileMode::Pitch ? target.width :
                            util::AlignUp<size_t>(static_cast<size_t>(guest.dimensions.width) * guest.format->bpb, 64)};
                        const size_t rows{guest.tileConfig.mode == gpu::texture::TileMode::Pitch ? target.height :
                            util::AlignUp<size_t>(target.height, 8 * guest.tileConfig.blockHeight)};
                        const size_t slices{guest.tileConfig.mode == gpu::texture::TileMode::Pitch ? 1 :
                            util::AlignUp<size_t>(depth, guest.tileConfig.blockDepth)};
                        constexpr size_t maximumLayerSize{std::numeric_limits<u32>::max()};
                        if (rowBytes > maximumLayerSize / rows || rowBytes * rows > maximumLayerSize / slices)
                            throw exception("RT extent exceeds the layer-size calculation's u32 range");
                    }
                    // Widen before multiplying; GetSize itself uses a u32 product.
                    destinationSize = static_cast<size_t>(guest.GetLayerStride()) * guest.layerCount;
                    extentKnown = destinationSize != 0;
                }
            } catch (const exception &error) {
                if (sequence <= 16)
                    LOGI("MINIRD_DRAW_TEXTURE_EXTENT_UNAVAILABLE event={} reason={}", event, error.what());
            }
        }

        if (!extentKnown)
            diagnostic::DrawTextureUnknownExtents().fetch_add(1, std::memory_order_relaxed);

        // Translate every segment and compare whole ranges, including a span
        // starting before the surface. GPU-VA aliases resolve through the GMMU.
        constexpr u64 addressSpaceSize{1ULL << GmmuAddressSpaceBits};
        if (destination >= addressSpaceSize || !destinationSize || destinationSize > addressSpaceSize - destination) {
            LOGI("MINIRD_DRAW_TEXTURE_DEST_UNAVAILABLE event={} dst_iova=0x{:X} extent={}", event, destination, destinationSize);
            return;
        }
        const auto mappings{channelCtx.asCtx->gmmu.TranslateRange(destination, destinationSize)};
        std::optional<diagnostic::PresentRegion> present;
        for (const auto &mapping : mappings) {
            if (auto candidate{diagnostic::FindPresentationOverlap(mapping.data(), mapping.size())};
                candidate && (!present || candidate->registration > present->registration))
                present = candidate;
        }
        const auto candidates{present ? diagnostic::DrawTexturePresentCandidates().fetch_add(1, std::memory_order_relaxed) + 1 : 0};
        if (sequence > 16 && !std::has_single_bit(sequence) && (!present || candidates > 16))
            return;

        LOGI("MINIRD_DRAW_TEXTURE_TRIGGER version={} event={} channel=0x{:X} seq={} submission={} "
             "src_texture={} src_sampler={} sampler_binding={} "
             "dst_xy=0x{:08X},0x{:08X} dst_wh=0x{:08X},0x{:08X} "
             "src_xy=0x{:08X},0x{:08X} gradients=0x{:08X},0x{:08X},0x{:08X},0x{:08X} "
             "ct_select=0x{:08X} dst_target={} dst_iova=0x{:X} dst_wh_regs={}x{} "
             "dst_format=0x{:X} dst_memory=0x{:08X} dst_layers_or_depth={} dst_layer={} "
             "dst_array_pitch={} extent={} extent_known={} maps={} present_candidate={} "
             "nvmap={} present_base=0x{:X} present_registration={} "
             "window_origin=0x{:08X} surface_clip=0x{:08X},0x{:08X} "
             "render_enable={} render_override={}",
             diagnostic::DrawTextureProbeVersion, event, reinterpret_cast<uintptr_t>(&channelCtx), sequence,
             channelCtx.executor.submissionNumber, registers.raw[0x429], registers.raw[0x428], registers.raw[0x48D],
             registers.raw[0x420], registers.raw[0x421], registers.raw[0x422], registers.raw[0x423],
             registers.raw[0x42A], registers.raw[0x42B], registers.raw[0x424], registers.raw[0x425],
             registers.raw[0x426], registers.raw[0x427], registers.raw[0x487], targetIndex, destination,
             target.width, target.height, static_cast<u32>(target.format), registers.raw[0x205 + targetIndex * 0x10],
             static_cast<u32>(target.thirdDimension), target.layerOffset, target.ArrayPitch(), destinationSize, extentKnown,
             mappings.size(), present.has_value(), present ? present->nvmapId : 0,
             present ? present->base : 0, present ? present->registration : 0,
             registers.raw[0x4EB], registers.raw[0x3FD], registers.raw[0x3FE],
             static_cast<u32>(registers.renderEnable->mode), static_cast<u32>(registers.renderEnableOverride->mode));
        for (size_t i{}; i < mappings.size(); ++i) {
            const auto &mapping{mappings[i]};
            const auto candidate{diagnostic::FindPresentationOverlap(mapping.data(), mapping.size())};
            if (i < 8 || candidate)
                LOGI("MINIRD_DRAW_TEXTURE_MAP event={} segment={} host=0x{:X} bytes={} "
                     "nvmap={} present_base=0x{:X}", event, i, reinterpret_cast<uintptr_t>(mapping.data()),
                     mapping.size(), candidate ? candidate->nvmapId : 0, candidate ? candidate->base : 0);
        }

        // Read only the 32-byte TIC descriptor after validating its mapping.
        // This does not sample pixels or create a host texture for the source.
        const u32 textureIndex{registers.raw[0x429]};
        const u64 poolAddress{registers.texHeaderPool->offset};
        const u64 headerOffset{static_cast<u64>(textureIndex) * sizeof(gpu::interconnect::TextureImageControl)};
        if (poolAddress > addressSpaceSize - sizeof(gpu::interconnect::TextureImageControl) ||
            textureIndex > registers.texHeaderPool->maximumIndex ||
            headerOffset > addressSpaceSize - sizeof(gpu::interconnect::TextureImageControl) - poolAddress) {
            LOGI("MINIRD_DRAW_TEXTURE_SOURCE_UNAVAILABLE event={} tic_pool=0x{:X} index={} maximum={}", event, poolAddress, textureIndex, registers.texHeaderPool->maximumIndex);
            return;
        }
        const u64 headerAddress{poolAddress + headerOffset};
        const auto headerMappings{channelCtx.asCtx->gmmu.TranslateRange(headerAddress, sizeof(gpu::interconnect::TextureImageControl))};
        for (const auto &mapping : headerMappings) {
            if (!mapping.valid()) {
                LOGI("MINIRD_DRAW_TEXTURE_SOURCE_UNAVAILABLE event={} tic_iova=0x{:X} unmapped=true", event, headerAddress);
                return;
            }
        }
        gpu::interconnect::TextureImageControl tic{};
        try {
            tic = channelCtx.asCtx->gmmu.Read<gpu::interconnect::TextureImageControl>(headerAddress);
        } catch (const exception &error) {
            // AsGpu can unmap between TranslateRange and Read. A page fault
            // from this optional metadata read must not kill the guest. No GPU
            // execution or Vulkan operation is inside this diagnostic catch.
            LOGI("MINIRD_DRAW_TEXTURE_SOURCE_UNAVAILABLE event={} tic_iova=0x{:X} reason={}", event, headerAddress, error.what());
            return;
        }
        if (tic.Iova() >= addressSpaceSize) {
            LOGI("MINIRD_DRAW_TEXTURE_SOURCE_UNAVAILABLE event={} src_iova=0x{:X} out_of_range=true", event, tic.Iova());
            return;
        }
        const auto sourceMappings{channelCtx.asCtx->gmmu.TranslateRange(tic.Iova(), 1)};
        LOGI("MINIRD_DRAW_TEXTURE_SOURCE event={} tic_iova=0x{:X} src_iova=0x{:X} src_host=0x{:X} "
             "src_wh={}x{} src_depth={} src_layer={} src_header={} src_format=0x{:08X} "
             "src_tile=0x{:04X} src_view=0x{:08X}", event, headerAddress, tic.Iova(),
             sourceMappings.empty() ? 0 : reinterpret_cast<uintptr_t>(sourceMappings.front().data()),
             static_cast<u32>(tic.widthMinusOne) + 1, static_cast<u32>(tic.heightMinusOne) + 1,
             static_cast<u32>(tic.depthMinusOne) + 1, tic.BaseLayer(), static_cast<u32>(tic.headerType),
             tic.formatWord.Raw(), tic.tileConfig.raw, tic.viewConfig.raw);

        // DRAW_TEXTURE has explicit TIC and TSC IDs. The experimental handler
        // uses the programmable pipeline's linked-TSC rule instead, choosing
        // textureIndex when ViaHeaderBinding is enabled. Read only these two
        // descriptors to determine whether that difference matters here.
        // This shares the existing event print gate and performs no sampler
        // creation, texture lookup, command submission or pixel readback.
        const u32 samplerIndex{registers.raw[0x428]};
        const u64 samplerPoolAddress{registers.texSamplerPool->offset};
        const u32 samplerMaximum{registers.samplerBinding->value == type::SamplerBinding::Value::ViaHeaderBinding ?
            registers.texHeaderPool->maximumIndex : registers.texSamplerPool->maximumIndex};
        const std::array<u32, 2> samplerIndices{samplerIndex, textureIndex};
        for (size_t i{}; i < samplerIndices.size(); ++i) {
            const u32 index{samplerIndices[i]};
            if (i && index == samplerIndices[0])
                continue;
            using SamplerWords = std::array<u32, 8>;
            const u64 descriptorOffset{static_cast<u64>(index) * sizeof(SamplerWords)};
            if (samplerPoolAddress > addressSpaceSize - sizeof(SamplerWords) || index > samplerMaximum ||
                descriptorOffset > addressSpaceSize - sizeof(SamplerWords) - samplerPoolAddress) {
                LOGI("MINIRD_DRAW_TEXTURE_SAMPLER_UNAVAILABLE event={} index={} pool=0x{:X} maximum={}",
                     event, index, samplerPoolAddress, samplerMaximum);
                continue;
            }
            const u64 descriptorAddress{samplerPoolAddress + descriptorOffset};
            const auto descriptorMappings{channelCtx.asCtx->gmmu.TranslateRange(descriptorAddress, sizeof(SamplerWords))};
            if (std::any_of(descriptorMappings.begin(), descriptorMappings.end(), [](const auto &mapping) { return !mapping.valid(); })) {
                LOGI("MINIRD_DRAW_TEXTURE_SAMPLER_UNAVAILABLE event={} index={} tsc_iova=0x{:X} unmapped=true",
                     event, index, descriptorAddress);
                continue;
            }
            SamplerWords words{};
            try {
                words = channelCtx.asCtx->gmmu.Read<SamplerWords>(descriptorAddress);
            } catch (const exception &error) {
                // Only this optional descriptor read can be caught here.
                LOGI("MINIRD_DRAW_TEXTURE_SAMPLER_UNAVAILABLE event={} index={} tsc_iova=0x{:X} reason={}",
                     event, index, descriptorAddress, error.what());
                continue;
            }
            LOGI("MINIRD_DRAW_TEXTURE_SAMPLER event={} explicit_index={} linked_index={} index={} "
                 "tsc_iova=0x{:X} tsc_maximum={} effective_maximum={} "
                 "words=0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X},0x{:08X}",
                 event, samplerIndex, textureIndex, index, descriptorAddress,
                 static_cast<u32>(registers.texSamplerPool->maximumIndex), samplerMaximum,
                 words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
        }
    }

    __attribute__((always_inline)) void Maxwell3D::FlushDeferredDraw() {
        if (batchEnableState.drawActive) {
            batchEnableState.drawActive = false;
            if (CheckRenderEnable())
                interconnect.Draw(deferredDraw.drawTopology, *registers.streamOutputEnable, deferredDraw.indexed, deferredDraw.drawCount, deferredDraw.drawFirst, deferredDraw.instanceCount, deferredDraw.drawBaseVertex, deferredDraw.drawBaseInstance);
            deferredDraw.instanceCount = 1;
        }
    }

    __attribute__((always_inline)) void Maxwell3D::FlushInlineIndexDraw() {
       if (batchEnableState.inlineIndexActive) {
           batchEnableState.inlineIndexActive = false;
           if (batchInlineIndex.totalCount != 0 && batchInlineIndex.indices.size() > batchInlineIndex.totalCount)
               batchInlineIndex.indices.resize(batchInlineIndex.totalCount);
           if (!batchInlineIndex.indices.empty()) {
               if (CheckRenderEnable())
                   interconnect.DrawWithInlineIndex(ApplyTopologyOverride(registers.begin->op), *registers.streamOutputEnable,
                                                     span(batchInlineIndex.indices).cast<u8>(),
                                                     type::IndexBuffer::IndexSize::TwoBytes,
                                                     static_cast<u32>(batchInlineIndex.indices.size()), 1);
           }
           batchInlineIndex.Reset();
        }
    }
   
    __attribute__((always_inline)) void Maxwell3D::HandleMethod(u32 method, u32 argument) {
         if (method == ENGINE_STRUCT_OFFSET(mme, shadowRamControl)) [[unlikely]] {
            shadowRegisters.raw[method] = registers.raw[method] = argument;
            return;
        }

        if (shadowRegisters.mme->shadowRamControl == type::MmeShadowRamControl::MethodTrack || shadowRegisters.mme->shadowRamControl == type::MmeShadowRamControl::MethodTrackWithFilter) [[unlikely]]
            shadowRegisters.raw[method] = argument;
        else if (shadowRegisters.mme->shadowRamControl == type::MmeShadowRamControl::MethodReplay) [[unlikely]]
            argument = shadowRegisters.raw[method];


        bool redundant{registers.raw[method] == argument};
        u32 origRegisterValue{registers.raw[method]};
        registers.raw[method] = argument;

        // Diagnostic only: the DRAW_TEXTURE register block starts at 0x420,
        // and a write to its final word (SRC_Y0, 0x42B) triggers the draw.
        // Run before deferred-draw handling and independently of redundancy:
        // an identical SRC_Y0 write is still a new hardware draw trigger.
        if (method == 0x42B)
            TraceDrawTexture();

        if (batchEnableState.raw) {
            if (batchEnableState.constantBufferActive) {
                switch (method) {
                    // Add to the batch constant buffer update buffer
                    // Return early here so that any code below can rely on the fact that any cbuf updates will always be the first of a batch
                    #define LOAD_CONSTANT_BUFFER_CALLBACKS(z, index, data_)                \
                    ENGINE_STRUCT_ARRAY_CASE(loadConstantBuffer, data, index, { \
                        batchLoadConstantBuffer.buffer.push_back(argument);         \
                        registers.loadConstantBuffer->offset += 4;              \
                        return;                                                   \
                    })

                    BOOST_PP_REPEAT(16, LOAD_CONSTANT_BUFFER_CALLBACKS, 0)
                    #undef LOAD_CONSTANT_BUFFER_CALLBACKS
                    default:
                        // When a method other than constant buffer update is called submit our submit the previously built-up update as a batch
                        registers.raw[method] = origRegisterValue;
                        interconnect.DisableQuickConstantBufferBind();
                        interconnect.LoadConstantBuffer(batchLoadConstantBuffer.buffer, batchLoadConstantBuffer.startOffset);
                        batchEnableState.constantBufferActive = false;
                        batchLoadConstantBuffer.Reset();
                        registers.raw[method] = argument;
                        break; // Continue on here to handle the actual method
                }
            } else if (batchEnableState.drawActive) { // See DeferredDrawState comment for full details
                switch (method) {
                    ENGINE_CASE(begin, {
                        if (begin.instanceId == Registers::Begin::InstanceId::Subsequent) {
                            if (deferredDraw.drawTopology != begin.op &&
                                registers.primitiveTopologyControl->override == type::PrimitiveTopologyControl::Override::UseTopologyInBeginMethods)
                                LOGW("Vertex topology changed partway through instanced draw!");

                            deferredDraw.instanceCount++;
                        } else {
                            FlushDeferredDraw();
                            break; // This instanced draw is finished, continue on to handle the next draw
                        }

                        return;
                    })

                    // Can be ignored since we handle drawing in draw{Vertex,Index}Count
                    ENGINE_CASE(end, { return; })

                    // Draws here can be ignored since they're just repeats of the original instanced draw
                    ENGINE_CASE(drawVertexArray, {
                        if (!redundant)
                            LOGW("Vertex count changed partway through instanced draw!");
                        return;
                    })
                    ENGINE_CASE(drawIndexBuffer, {
                        if (!redundant)
                            LOGW("Index count changed partway through instanced draw!");
                        return;
                    })

                    ENGINE_CASE(drawVertexArrayBeginEndInstanceSubsequent, {
                        deferredDraw.instanceCount++;
                        return;
                    })

                    ENGINE_CASE(drawIndexBuffer32BeginEndInstanceSubsequent, {
                        deferredDraw.instanceCount++;
                        return;
                    })

                    ENGINE_CASE(drawIndexBuffer16BeginEndInstanceSubsequent, {
                        deferredDraw.instanceCount++;
                        return;
                    })

                    ENGINE_CASE(drawIndexBuffer8BeginEndInstanceSubsequent, {
                        deferredDraw.instanceCount++;
                        return;
                    })

                    // Once we stop calling draw methods flush the current draw since drawing is dependent on the register state not changing
                    default:
                        registers.raw[method] = origRegisterValue;
                        FlushDeferredDraw();
                        registers.raw[method] = argument;
                        break;
                   }
                } else if (batchEnableState.inlineIndexActive) {
                    switch (method) {
                    ENGINE_STRUCT_CASE(drawInlineIndex2X16, even, {
                        if (batchInlineIndex.firstWord && batchInlineIndex.skipFirstEven)
                            batchInlineIndex.indices.push_back(static_cast<u16>(registers.drawInlineIndex2X16->odd));
                        else {
                            batchInlineIndex.indices.push_back(static_cast<u16>(registers.drawInlineIndex2X16->even));
                            batchInlineIndex.indices.push_back(static_cast<u16>(registers.drawInlineIndex2X16->odd));
                        }
                        batchInlineIndex.firstWord = false;
                        return;
                     })
                     default:
                         registers.raw[method] = origRegisterValue;
                         FlushInlineIndexDraw();
                         registers.raw[method] = argument;
                         break;
                   }
              }
         }

        if (!redundant)
            dirtyManager.MarkDirty(method);

        switch (method) {
            ENGINE_STRUCT_CASE(mme, instructionRamLoad, {
                if (registers.mme->instructionRamPointer >= macroState.macroCode.size())
                    throw exception("Macro memory is full!");

                macroState.macroCode[registers.mme->instructionRamPointer++] = instructionRamLoad;
                macroState.Invalidate();

                // Wraparound writes
                // This works on HW but will also generate an error interrupt
                registers.mme->instructionRamPointer %= macroState.macroCode.size();
            })

            ENGINE_STRUCT_CASE(mme, startAddressRamLoad, {
                if (registers.mme->startAddressRamPointer >= macroState.macroPositions.size())
                    throw exception("Maximum amount of macros reached!");

                macroState.macroPositions[registers.mme->startAddressRamPointer++] = startAddressRamLoad;
                macroState.Invalidate();
            })

            ENGINE_STRUCT_CASE(i2m, launchDma, {
                FlushEngineState();
                i2m.LaunchDma(*registers.i2m);
            })

            ENGINE_STRUCT_CASE(i2m, loadInlineData, {
                i2m.LoadInlineData(*registers.i2m, loadInlineData);
            })

            ENGINE_CASE(clearReportValue, {
                interconnect.ResetCounter(clearReportValue.type);
            })

            ENGINE_CASE(syncpointAction, {
                LOGD("Increment syncpoint: {}", static_cast<u16>(syncpointAction.id));
                channelCtx.executor.AddDeferredAction([=, syncpoints = &this->syncpoints, index = syncpointAction.id]() {
                    syncpoints->at(index).host.Increment();
                });
                syncpoints.at(syncpointAction.id).guest.Increment();
            })

            ENGINE_CASE(clearSurface, {
                if (CheckRenderEnable())
                    interconnect.Clear(clearSurface);
            })

            ENGINE_CASE(begin, {
                // If we reach here then we aren't in a deferred draw so theres no need to flush anything
                if (begin.instanceId == Registers::Begin::InstanceId::Subsequent)
                    deferredDraw.instanceCount++;
                else
                    deferredDraw.instanceCount = 1;
            })

            ENGINE_STRUCT_CASE(drawVertexArray, count, {
                // Defer the draw until the first non-draw operation to allow for detecting instanced draws (see DeferredDrawState comment)
                deferredDraw.Set(count, *registers.vertexArrayStart, 0,
                                 *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(registers.begin->op),
                                 false);
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawVertexArrayBeginEndInstanceFirst, {
                deferredDraw.Set(drawVertexArrayBeginEndInstanceFirst.count, drawVertexArrayBeginEndInstanceFirst.startIndex, 0,
                                 *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawVertexArrayBeginEndInstanceFirst.topology),
                                 false);
                deferredDraw.instanceCount = 1;
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawVertexArrayBeginEndInstanceSubsequent, {
                deferredDraw.Set(drawVertexArrayBeginEndInstanceSubsequent.count, drawVertexArrayBeginEndInstanceSubsequent.startIndex, 0,
                                 *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawVertexArrayBeginEndInstanceSubsequent.topology),
                                 false);
                deferredDraw.instanceCount++;
                batchEnableState.drawActive = true;
            })
           
            ENGINE_CASE(inlineIndex2X16Align, {
                    batchInlineIndex.indices.clear();
                    batchInlineIndex.totalCount = registers.inlineIndex2X16Align->count;
                    batchInlineIndex.skipFirstEven = registers.inlineIndex2X16Align->startOdd;
                    batchInlineIndex.firstWord = true;
            })
          
            ENGINE_STRUCT_CASE(drawInlineIndex4X8, index0, {
                    throw exception("drawInlineIndex4X8 not implemented!");
            })
          
            ENGINE_STRUCT_CASE(drawInlineIndex2X16, even, {
                    if (batchInlineIndex.firstWord && batchInlineIndex.skipFirstEven)
                        batchInlineIndex.indices.push_back(static_cast<u16>(registers.drawInlineIndex2X16->odd));
                    else {
                        batchInlineIndex.indices.push_back(static_cast<u16>(registers.drawInlineIndex2X16->even));
                        batchInlineIndex.indices.push_back(static_cast<u16>(registers.drawInlineIndex2X16->odd));
                    }
                    batchInlineIndex.firstWord = false;
                    batchEnableState.inlineIndexActive = true;
                })

            ENGINE_STRUCT_CASE(drawZeroIndex, count, {
                throw exception("drawZeroIndex not implemented!");
            })

            ENGINE_STRUCT_CASE(drawAuto, byteCount, {
                throw exception("drawAuto not implemented!");
            })

            ENGINE_CASE(drawInlineIndex, {
                throw exception("drawInlineIndex not implemented!");
            })

            ENGINE_STRUCT_CASE(drawIndexBuffer, count, {
                // Defer the draw until the first non-draw operation to allow for detecting instanced draws (see DeferredDrawState comment)
                deferredDraw.Set(count, registers.indexBuffer->first,
                                 *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(registers.begin->op),
                                 true);
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawIndexBuffer32BeginEndInstanceFirst, {
                deferredDraw.Set(drawIndexBuffer32BeginEndInstanceFirst.count, drawIndexBuffer32BeginEndInstanceFirst.first,
                                 *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawIndexBuffer32BeginEndInstanceFirst.topology),
                                 true);
                deferredDraw.instanceCount = 1;
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawIndexBuffer16BeginEndInstanceFirst, {
                deferredDraw.Set(drawIndexBuffer16BeginEndInstanceFirst.count, drawIndexBuffer16BeginEndInstanceFirst.first,
                                 *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawIndexBuffer16BeginEndInstanceFirst.topology),
                                 true);
                deferredDraw.instanceCount = 1;
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawIndexBuffer8BeginEndInstanceFirst, {
                deferredDraw.Set(drawIndexBuffer8BeginEndInstanceFirst.count, drawIndexBuffer8BeginEndInstanceFirst.first,
                                 *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawIndexBuffer8BeginEndInstanceFirst.topology),
                                 true);
                deferredDraw.instanceCount = 1;
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawIndexBuffer32BeginEndInstanceSubsequent, {
                deferredDraw.Set(drawIndexBuffer32BeginEndInstanceSubsequent.count, drawIndexBuffer32BeginEndInstanceSubsequent.first,
                                 *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawIndexBuffer32BeginEndInstanceSubsequent.topology),
                                 true);
                deferredDraw.instanceCount++;
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawIndexBuffer16BeginEndInstanceSubsequent, {
                deferredDraw.Set(drawIndexBuffer16BeginEndInstanceSubsequent.count, drawIndexBuffer16BeginEndInstanceSubsequent.first,
                                 *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawIndexBuffer16BeginEndInstanceSubsequent.topology),
                                 true);
                deferredDraw.instanceCount++;
                batchEnableState.drawActive = true;
            })

            ENGINE_CASE(drawIndexBuffer8BeginEndInstanceSubsequent, {
                deferredDraw.Set(drawIndexBuffer8BeginEndInstanceSubsequent.count, drawIndexBuffer8BeginEndInstanceSubsequent.first,
                                 *registers.globalBaseVertexIndex, *registers.globalBaseInstanceIndex,
                                 ApplyTopologyOverride(drawIndexBuffer8BeginEndInstanceSubsequent.topology),
                                 true);
                deferredDraw.instanceCount++;
                batchEnableState.drawActive = true;
            })

            ENGINE_STRUCT_CASE(semaphore, info, {
                if (info.reductionEnable)
                    LOGW("Semaphore reduction is unimplemented!");

                switch (info.op) {
                    case type::SemaphoreInfo::Op::Release:
                        channelCtx.executor.AddDeferredAction([=, this, semaphore = *registers.semaphore]() {
                            WriteSemaphoreResult(semaphore, semaphore.payload);
                        });
                        break;

                    case type::SemaphoreInfo::Op::Counter: {
                        switch (info.counterType) {
                            case type::SemaphoreInfo::CounterType::Zero:
                                channelCtx.executor.AddDeferredAction([=, this, semaphore = *registers.semaphore]() {
                                    WriteSemaphoreResult(semaphore, semaphore.payload);
                                });
                                break;
                            case type::SemaphoreInfo::CounterType::SamplesPassed:
                                // Return a fake result for now
                                interconnect.Query({registers.semaphore->address}, info.counterType,
                                                   registers.semaphore->info.structureSize == type::SemaphoreInfo::StructureSize::FourWords ?
                                                   GetGpuTimeTicks() : std::optional<u64>{});
                                break;

                            default:
                                LOGD("Unsupported semaphore counter type: 0x{:X}", static_cast<u8>(info.counterType));
                                break;
                        }
                        break;
                    }

                    default:
                        LOGW("Unsupported semaphore operation: 0x{:X}", static_cast<u8>(info.op));
                        break;
                }
            })

            ENGINE_ARRAY_CASE(firmwareCall, 4, {
                registers.raw[0xD00] = 1;
            })

            ENGINE_CASE(invalidateSamplerCacheAll, {
                channelCtx.executor.Submit();
            })

            ENGINE_CASE(invalidateTextureHeaderCacheAll, {
                channelCtx.executor.Submit();
            })

            // Begin a batch constant buffer update, this case will never be reached if a batch update is currently active
            #define LOAD_CONSTANT_BUFFER_CALLBACKS(z, index, data_)                                      \
            ENGINE_STRUCT_ARRAY_CASE(loadConstantBuffer, data, index, {                       \
                batchLoadConstantBuffer.startOffset = registers.loadConstantBuffer->offset;              \
                batchLoadConstantBuffer.buffer.push_back(data);                                          \
                batchEnableState.constantBufferActive = true;                                        \
                registers.loadConstantBuffer->offset += 4;                                    \
            })

            BOOST_PP_REPEAT(16, LOAD_CONSTANT_BUFFER_CALLBACKS, 0)
            #undef LOAD_CONSTANT_BUFFER_CALLBACKS

            #define PIPELINE_CALLBACKS(z, idx, data)                                                                                         \
                ENGINE_ARRAY_STRUCT_CASE(bindGroups, idx, constantBuffer, {                                                                  \
                    interconnect.BindConstantBuffer(static_cast<type::ShaderStage>(idx), constantBuffer.shaderSlot, constantBuffer.valid);   \
                })

            BOOST_PP_REPEAT(5, PIPELINE_CALLBACKS, 0)
            static_assert(type::ShaderStageCount == 5 && type::ShaderStageCount < BOOST_PP_LIMIT_REPEAT);
            #undef PIPELINE_CALLBACKS
            default:
                break;
        }
    }

    void Maxwell3D::WriteSemaphoreResult(const Registers::Semaphore &semaphore, u64 result) {
        switch (semaphore.info.structureSize) {
            case type::SemaphoreInfo::StructureSize::OneWord:
                channelCtx.asCtx->gmmu.Write(semaphore.address, static_cast<u32>(result));
                LOGD("address: 0x{:X} payload: {}", semaphore.address, result);
                break;

            case type::SemaphoreInfo::StructureSize::FourWords: {
                // Write timestamp first to ensure correct ordering
                u64 timestamp{GetGpuTimeTicks()};
                channelCtx.asCtx->gmmu.Write(semaphore.address + 8, timestamp);
                channelCtx.asCtx->gmmu.Write(semaphore.address, result);
                LOGD("address: 0x{:X} payload: {} timestamp: {}", semaphore.address, result, timestamp);

                break;
            }
        }
    }

    void Maxwell3D::FlushEngineState() {
        FlushDeferredDraw();
        FlushInlineIndexDraw();
   
            if (batchEnableState.constantBufferActive) {
            interconnect.LoadConstantBuffer(batchLoadConstantBuffer.buffer, batchLoadConstantBuffer.startOffset);
            batchEnableState.constantBufferActive = false;
            batchLoadConstantBuffer.Reset();
        }

        interconnect.DisableQuickConstantBufferBind();
    }

    __attribute__((always_inline)) void Maxwell3D::CallMethod(u32 method, u32 argument) {
        LOGV("Called method in Maxwell 3D: 0x{:X} args: 0x{:X}", method, argument);

        HandleMethod(method, argument);
    }

    void Maxwell3D::CallMethodBatchNonInc(u32 method, span<u32> arguments) {
        switch (method) {
            case ENGINE_STRUCT_OFFSET(i2m, loadInlineData):
                i2m.LoadInlineData(*registers.i2m, arguments);
                return;
            default:
                break;
        }

        for (u32 argument : arguments)
            HandleMethod(method, argument);
    }

    void Maxwell3D::CallMethodFromMacro(u32 method, u32 argument) {
        HandleMethod(method, argument);
    }

    u32 Maxwell3D::ReadMethodFromMacro(u32 method) {
        return registers.raw[method];
    }

    void Maxwell3D::DrawInstanced(u32 drawTopology, u32 vertexArrayCount, u32 instanceCount, u32 vertexArrayStart, u32 globalBaseInstanceIndex) {
        FlushEngineState();

        auto topology{static_cast<type::DrawTopology>(drawTopology)};
        registers.globalBaseInstanceIndex = globalBaseInstanceIndex;
        registers.vertexArrayStart = vertexArrayStart;
        if (CheckRenderEnable())
            interconnect.Draw(topology, *registers.streamOutputEnable, false, vertexArrayCount, vertexArrayStart, instanceCount, 0, globalBaseInstanceIndex);
        registers.globalBaseInstanceIndex = 0;
    }

    void Maxwell3D::DrawIndexedInstanced(u32 drawTopology, u32 indexBufferCount, u32 instanceCount, u32 globalBaseVertexIndex, u32 indexBufferFirst, u32 globalBaseInstanceIndex) {
        FlushEngineState();
        auto topology{static_cast<type::DrawTopology>(drawTopology)};
        if (CheckRenderEnable())
            interconnect.Draw(topology, *registers.streamOutputEnable, true, indexBufferCount, indexBufferFirst, instanceCount, globalBaseVertexIndex, globalBaseInstanceIndex);
    }

    void Maxwell3D::DrawIndexedIndirect(u32 drawTopology, span<u8> indirectBuffer, u32 count, u32 stride) {
        FlushEngineState();
        auto topology{static_cast<type::DrawTopology>(drawTopology)};
        if (CheckRenderEnable())
            interconnect.DrawIndirect(topology, *registers.streamOutputEnable, true, indirectBuffer, count, stride);
    }

}
