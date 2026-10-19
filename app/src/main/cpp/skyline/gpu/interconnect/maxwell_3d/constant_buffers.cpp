// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <limits>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "constant_buffers.h"
#include "gpu/buffer.h"

namespace skyline::gpu::interconnect::maxwell3d {
    void ConstantBufferSelectorState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, constantBufferSelector);
    }

    ConstantBufferSelectorState::ConstantBufferSelectorState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    void ConstantBufferSelectorState::Flush(InterconnectContext &ctx, size_t minSize) {
        const auto &selector{engine->constantBufferSelector};
        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        const auto capacity{std::max<size_t>(minSize, selector.size)};
        const auto generation{gmmu.GetMappingGeneration()};
        size_t available{};
        // The declaration is often the default 64 KiB capacity. Only the actual
        // access is required; clip optional capacity at the first unmapped byte.
        for (const auto &mapping : gmmu.TranslateMappings(selector.address, capacity)) {
            if (mapping.kind == MemoryMappingKind::Unmapped)
                break;
            available += mapping.memory.size();
        }
        mappingAddressSpace = &gmmu;
        mappingGeneration = generation;
        selectorAddress = selector.address;
        selectorCapacity = capacity;
        mappedPrefixSize = available;
        view.Update(ctx, selector.address, available >= minSize ? available : 0, false);
    }

    bool ConstantBufferSelectorState::Refresh(InterconnectContext &ctx, size_t minSize) {
        const auto &selector{engine->constantBufferSelector};
        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        if (mappingAddressSpace != &gmmu || mappingGeneration != gmmu.GetMappingGeneration() ||
            selectorAddress != selector.address || selectorCapacity != std::max<size_t>(minSize, selector.size)) {
            Flush(ctx, minSize);
        } else {
            // Keep the normal cache access path, including alias preflight, even
            // when the selector's optional capacity needs no new translation.
            view.Update(ctx, selector.address, mappedPrefixSize >= minSize ? mappedPrefixSize : 0, false);
        }
        return false;
    }

    void ConstantBufferSelectorState::PurgeCaches() {
        mappingAddressSpace = nullptr;
        view.PurgeCaches();
    }

    ConstantBuffers::ConstantBuffers(DirtyManager &manager, const ConstantBufferSelectorState::EngineRegisters &constantBufferSelectorRegisters) : selectorState{manager, constantBufferSelectorRegisters} {}

    void ConstantBuffers::MarkAllDirty() {
        selectorState.MarkDirty(true);
    }

    void ConstantBuffers::Load(InterconnectContext &ctx, span<u32> data, u32 offset) {
        TRACE_EVENT("gpu", "ConstantBuffers::Load");

        if (data.size_bytes() > std::numeric_limits<size_t>::max() - offset)
            throw exception("Constant buffer load range overflows its address space");
        const auto requiredSize{static_cast<size_t>(offset) + data.size_bytes()};
        auto &view{*selectorState.UpdateGet(ctx, requiredSize).view};
        if (!view)
            throw exception("Constant buffer load exceeds its mapped selector");
        auto srcCpuBuf{data.cast<u8>()};

        ContextLock lock{ctx.executor.tag, view};

        // First attempt the write without setting up the gpu copy callback as a fast path
        if (view.Write(srcCpuBuf, offset, ctx.executor.usageTracker)) [[unlikely]] {
            // Store callback data in a stack allocated struct to avoid heap allocation for the gpu copy callback lambda
            struct GpuCopyCallbackData {
                InterconnectContext &ctx;
                span<u8> srcCpuBuf;
                u32 offset;
                ContextLock<BufferView> &lock;
                BufferView &view;
            } callbackData{ctx, srcCpuBuf, offset, lock, view};

            view.Write(srcCpuBuf, offset, ctx.executor.usageTracker, [&callbackData]() {
                callbackData.ctx.executor.AttachLockedBufferView(callbackData.view, std::move(callbackData.lock));
                // This will prevent any CPU accesses to backing for the duration of the usage
                callbackData.view.GetBuffer()->BlockAllCpuBackingWrites();

                auto srcGpuAllocation{callbackData.ctx.gpu.megaBufferAllocator.Push(callbackData.ctx.executor.cycle, callbackData.srcCpuBuf)};
                callbackData.ctx.executor.AddCheckpoint("Before constant buffer load");
                callbackData.ctx.executor.AddOutsideRpCommand([=, srcCpuBuf = callbackData.srcCpuBuf, view = callbackData.view, offset = callbackData.offset](vk::raii::CommandBuffer &commandBuffer, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
                    auto binding{view.GetBinding(gpu)};
                    vk::BufferCopy copyRegion{
                        .size = srcCpuBuf.size_bytes(),
                        .srcOffset = srcGpuAllocation.offset,
                        .dstOffset = binding.offset + offset
                    };
                    commandBuffer.copyBuffer(srcGpuAllocation.buffer, binding.buffer, copyRegion);
                    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, vk::MemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                    }, {}, {});
                });
                callbackData.ctx.executor.AddCheckpoint("After constant buffer load");
            });
        }
    }

   void ConstantBuffers::Bind(InterconnectContext &ctx, engine::ShaderStage stage, size_t index) {
      auto &view{*selectorState.UpdateGet(ctx).view};
      if (!view)
        throw exception("Constant buffer selector is not mapped");

        boundConstantBuffers[static_cast<size_t>(stage)][index] = {view};

        // Forçando o caminho lento para testar os vértices do DBFZ
        DisableQuickBind(); 
            }

    void ConstantBuffers::Unbind(engine::ShaderStage stage, size_t index) {
        boundConstantBuffers[static_cast<size_t>(stage)][index] = {};
    }

    void ConstantBuffers::ResetQuickBind() {
        quickBindEnabled = true;
        quickBind.reset();
    }

    void ConstantBuffers::DisableQuickBind() {
        quickBindEnabled = false;
        quickBind.reset();
    }
}
