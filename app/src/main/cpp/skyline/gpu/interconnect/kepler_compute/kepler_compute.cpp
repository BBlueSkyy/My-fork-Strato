// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/Ryujinx/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/interconnect/command_executor.h>
#include <gpu/interconnect/common/state_updater.h>
#include <soc/gm20b/channel.h>
#include "pipeline_state.h"
#include "kepler_compute.h"
#include "dispatch_limits.h"
#include <atomic>

namespace skyline::gpu::interconnect::kepler_compute {

    static std::array<u32, 3> GetComputeDispatchLimits(const vk::raii::PhysicalDevice &device) {
        const auto limits{device.getProperties().limits.maxComputeWorkGroupCount};
        return {limits[0], limits[1], limits[2]};
    }

    KeplerCompute::KeplerCompute(GPU &gpu,
                                 soc::gm20b::ChannelContext &channelCtx,
                                 TrapManager &trap,
                                 kernel::MemoryManager &memoryManager,
                                 DirtyManager &manager,
                                 const EngineRegisterBundle &registerBundle)
        : ctx{channelCtx, channelCtx.executor, gpu, trap, memoryManager},
          maxComputeWorkGroupCount{GetComputeDispatchLimits(gpu.vkPhysicalDevice)},
          pipelineState{manager, registerBundle.pipelineStateRegisters},
          samplers{manager, registerBundle.samplerPoolRegisters},
          textures{manager, registerBundle.texturePoolRegisters} {
        ctx.executor.AddFlushCallback([this] {
            pipelineState.PurgeCaches();
            constantBuffers.MarkAllDirty();
            samplers.MarkAllDirty();
            textures.MarkAllDirty();
        });
    }

    void KeplerCompute::Dispatch(const QMD &qmd, u64 qmdAddress) {
        // An invalid QMD must not become an invalid Vulkan command. Do not
        // clamp the guest dispatch: that would silently execute a different grid.
        // Report it before descriptors, pipelines or command buffers are changed.
        const std::array<u32, 3> grid{qmd.ctaRasterWidth, qmd.ctaRasterHeight, qmd.ctaRasterDepth};
        if (!IsComputeGridWithinLimits(grid, maxComputeWorkGroupCount)) {
            static std::atomic<u32> invalidQmdCount{0};
            const u32 count{invalidQmdCount.fetch_add(1, std::memory_order_relaxed) + 1};
            // Bound diagnostic spam if the guest repeatedly submits a bad QMD.
            if (count <= 8 || (count & (count - 1)) == 0) {
                LOGE("[GPU-QMD] Rejecting invalid compute dispatch count={} iova=0x{:X} grid=({},{},{}) limits=({},{},{}) qmdVersion={}.{} programOffset=0x{:X} threads=({},{},{}) cbValid=0x{:X}",
                     count, qmdAddress, qmd.ctaRasterWidth, qmd.ctaRasterHeight, qmd.ctaRasterDepth,
                     maxComputeWorkGroupCount[0], maxComputeWorkGroupCount[1], maxComputeWorkGroupCount[2],
                     qmd.qmdMajorVersion, qmd.qmdVersion, qmd.programOffset,
                     qmd.ctaThreadDimension0, qmd.ctaThreadDimension1, qmd.ctaThreadDimension2,
                     qmd.constantBufferValid);
            }
            return;
        }

        ctx.executor.RunWithTextureBackingRetry([&] {
        if (ctx.gpu.traits.quirks.brokenComputeShaders)
            return;

        TRACE_EVENT("gpu", "KeplerCompute::Dispatch");

        StateUpdateBuilder builder{*ctx.executor.allocator};

        constantBuffers.Update(ctx, qmd);
        samplers.Update(ctx, qmd.samplerIndex == soc::gm20b::engine::kepler_compute::QMD::SamplerIndex::ViaHeaderIndex);
        auto *pipeline{pipelineState.Update(ctx, builder, textures, samplers, constantBuffers.boundConstantBuffers, qmd)};

        vk::PipelineStageFlags srcStageMask{}, dstStageMask{};
        auto *descUpdateInfo{pipeline->SyncDescriptors(ctx, constantBuffers.boundConstantBuffers, samplers, textures, srcStageMask, dstStageMask,
            {qmd.ctaRasterWidth, qmd.ctaRasterHeight, qmd.ctaRasterDepth})};
        builder.SetPipeline(*pipeline->compiledPipeline.pipeline, vk::PipelineBindPoint::eCompute);

        if (pipeline->compiledPipeline.usesPushDescriptors) {
            builder.SetDescriptorSetWithPush(descUpdateInfo);
        } else {
            auto set{std::make_shared<DescriptorAllocator::ActiveDescriptorSet>(ctx.gpu.descriptor.AllocateSet(descUpdateInfo->descriptorSetLayout))};

            builder.SetDescriptorSetWithUpdate(descUpdateInfo, set.get(), nullptr);
            ctx.executor.AttachDependency(set);
        }

        auto stateUpdater{builder.Build()};

        /**
         * @brief Struct that can be linearly allocated, holding all state for the draw to avoid a dynamic allocation with lambda captures
         */
        struct DrawParams {
            StateUpdater stateUpdater;
            std::array<u32, 3> dimensions;
            vk::PipelineStageFlags srcStageMask, dstStageMask;
        };
        auto *drawParams{ctx.executor.allocator->EmplaceUntracked<DrawParams>(DrawParams{stateUpdater, {qmd.ctaRasterWidth, qmd.ctaRasterHeight, qmd.ctaRasterDepth}, srcStageMask, dstStageMask})};


        ctx.executor.AddCheckpoint("Before dispatch");
        ctx.executor.AddOutsideRpCommand([drawParams](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
            drawParams->stateUpdater.RecordAll(gpu, commandBuffer);

            if (drawParams->srcStageMask && drawParams->dstStageMask)
                commandBuffer.pipelineBarrier(drawParams->srcStageMask, drawParams->dstStageMask, {}, {vk::MemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                }}, {}, {});

            commandBuffer.dispatch(drawParams->dimensions[0], drawParams->dimensions[1], drawParams->dimensions[2]);
        });
        ctx.executor.AddCheckpoint("After dispatch");

        });
    }
}
