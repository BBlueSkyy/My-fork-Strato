// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <limits>
#include <gpu/buffer_manager.h>
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
        // Constant buffer selector size is often left at the default value of 0x10000 which can end up being larger than the underlying mapping so avoid warning for split mappings here
        view.Update(ctx, selector.address, std::max<size_t>(minSize, selector.size), false);
    }

    bool ConstantBufferSelectorState::Refresh(InterconnectContext &ctx, size_t minSize) {
        const auto &selector{engine->constantBufferSelector};
        size_t selectorMinSize{std::max<size_t>(minSize, selector.size)};
        // Register values and view size may be unchanged across a remap.
        // Update validates the cached translator epoch without reallocating on
        // the stable path, and enrolls this use in operation publication.
        view.Update(ctx, selector.address, selectorMinSize, false);

        return false;
    }

    void ConstantBufferSelectorState::PurgeCaches() {
        view.PurgeCaches();
    }

    ConstantBuffers::ConstantBuffers(DirtyManager &manager, const ConstantBufferSelectorState::EngineRegisters &constantBufferSelectorRegisters) : selectorState{manager, constantBufferSelectorRegisters} {}

    void ConstantBuffers::MarkAllDirty() {
        selectorState.MarkDirty(true);
    }

    void ConstantBuffers::RefreshMappings() {
        for (auto &stage : boundConstantBuffers)
            for (auto &buffer : stage) buffer.RefreshMapping();
    }

    void ConstantBuffers::Load(InterconnectContext &ctx, span<u32> data, u32 offset) {
        ctx.executor.RunWithTextureBackingRetry([&] {
            TRACE_EVENT("gpu", "ConstantBuffers::Load");
            DisableQuickBind();
            if (data.size_bytes() > std::numeric_limits<size_t>::max() - offset)
                throw exception("Constant buffer load range overflows");
            const auto epoch{ctx.channelCtx.asCtx->gmmu.ReadMappingEpoch()};
            ctx.executor.StartTextureMapping(epoch);
            auto view{*selectorState.UpdateGet(ctx, offset + data.size_bytes()).view};
            if (!view || offset > view.size || data.size_bytes() > view.size - offset)
                throw exception("Constant buffer load exceeds mapped selector range");
            auto bytes{data.cast<u8>()};
            auto buffer{view.GetBuffer()->shared_from_this()};
            buffer->RequireRegionCoherence(ctx.executor.tag);
            ContextLock lock{ctx.executor.tag, *buffer};
            if (buffer->TryMappedWrite(bytes, view.GetOffset() + offset, ctx.executor.usageTracker, epoch)) return;
            ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
            ctx.executor.AttachBuffer(view);
            buffer->PrepareGpuWrite(ctx.executor.usageTracker);
            auto allocation{ctx.gpu.megaBufferAllocator.Push(ctx.executor.cycle, bytes)};
            auto write{view.SubView(offset, bytes.size())};
            ctx.executor.AddOutsideRpCommand([allocation, write](vk::raii::CommandBuffer &commands, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, vk::MemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite
                }, {}, {});
                auto binding{write.GetBinding(gpu)};
                commands.copyBuffer(allocation.buffer, binding.buffer, vk::BufferCopy{
                    .srcOffset = allocation.offset, .dstOffset = binding.offset, .size = write.size
                });
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, vk::MemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                }, {}, {});
            }, {}, span<BufferView>{write});
        });
    }

    void ConstantBuffers::Bind(InterconnectContext &ctx, engine::ShaderStage stage, size_t index) {
        auto &selector{selectorState.UpdateGet(ctx)};
        auto &view{*selector.view};
        if (!view)
            throw exception("Constant buffer selector is not mapped");

        boundConstantBuffers[static_cast<size_t>(stage)][index].BindMapped(ctx, selector.Address(), selector.Size(), selector.view);

        // The fast path can update one changed CBUF binding between draws.
        // After two distinct binds the descriptors need a full refresh.
        if (quickBindEnabled) {
            if (quickBind)
                DisableQuickBind();
            else
                quickBind = QuickBind{index, stage};
        }
    }

    void ConstantBuffers::Unbind(engine::ShaderStage stage, size_t index) {
        boundConstantBuffers[static_cast<size_t>(stage)][index] = {};
        // The quick updater expects a valid bound view, not an unbound slot.
        DisableQuickBind();
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
