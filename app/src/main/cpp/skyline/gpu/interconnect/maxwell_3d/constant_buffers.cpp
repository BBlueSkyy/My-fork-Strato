// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <atomic>
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
        const auto before{view.DescribeCache()};
        const auto revision{ctx.channelCtx.asCtx->gmmu.MappingRevision()};
        view.Update(ctx, selector.address, std::max<size_t>(minSize, selector.size), false);
        RecordDiagnostic(ctx, minSize, "flush", before, revision);
    }

    bool ConstantBufferSelectorState::Refresh(InterconnectContext &ctx, size_t minSize) {
        const auto &selector{engine->constantBufferSelector};
        size_t selectorMinSize{std::max<size_t>(minSize, selector.size)};
        if (view->size != selectorMinSize) {
            const auto before{view.DescribeCache()};
            const auto revision{ctx.channelCtx.asCtx->gmmu.MappingRevision()};
            view.Update(ctx, selector.address, selectorMinSize, false);
            RecordDiagnostic(ctx, minSize, "refresh", before, revision);
        }

        return false;
    }

    void ConstantBufferSelectorState::PurgeCaches() {
        const auto before{view.DescribeCache()};
        view.PurgeCaches();
        ++diagnosticPurges;
        diagnosticHistory[diagnosticEvents++ % diagnosticHistory.size()] = {"purge", 0, 0, 0, 0, 0, before, view.DescribeCache()};
    }

    void ConstantBufferSelectorState::RecordDiagnostic(InterconnectContext &ctx, size_t minSize, const char *kind,
            CachedMappedBufferView::CacheDiagnostic before, u64 revisionBefore) {
        const auto &selector{engine->constantBufferSelector};
        diagnosticHistory[diagnosticEvents++ % diagnosticHistory.size()] = {kind, selector.address, selector.size, minSize,
            revisionBefore, ctx.channelCtx.asCtx->gmmu.MappingRevision(), before, view.DescribeCache()};
    }

    void ConstantBufferSelectorState::DiagnoseBindFailure(InterconnectContext &ctx, engine::ShaderStage stage, size_t index) noexcept {
        try {
            static std::atomic<u32> failures{};
            if (failures.fetch_add(1, std::memory_order_relaxed) >= 2) return;
            const auto &selector{engine->constantBufferSelector};
            auto &gmmu{ctx.channelCtx.asCtx->gmmu};
            const auto cache{view.DescribeCache()};
            LOGE("CBUF_DIAG bind stage={} slot={} address=0x{:X} selectorSize=0x{:X} bindMinSize=0 channelSequence={} tagActive={} revision={} events={} purges={} selectorReceipt=none",
                static_cast<u32>(stage), index, static_cast<u64>(selector.address), static_cast<u32>(selector.size),
                ctx.channelCtx.channelSequenceNumber, static_cast<bool>(ctx.executor.tag), gmmu.MappingRevision(), diagnosticEvents, diagnosticPurges);
            LOGE("CBUF_DIAG cache start=0x{:X} end=0x{:X} physical=0x{:X} bytes=0x{:X} viewValid={} viewSize=0x{:X}",
                cache.blockStart, cache.blockEnd, cache.physical, cache.blockSize, cache.viewValid, cache.viewSize);
            const auto count{std::min<u64>(diagnosticEvents, diagnosticHistory.size())};
            for (u64 i{}; i < count; ++i) {
                const auto &event{diagnosticHistory[(diagnosticEvents - count + i) % diagnosticHistory.size()]};
                LOGE("CBUF_DIAG history serial={} kind={} address=0x{:X} selectorSize=0x{:X} minSize=0x{:X} revisions={}/{} beforeValid={} beforeSize=0x{:X} beforeBlock=[0x{:X},0x{:X}) beforePhysical=0x{:X} afterValid={} afterSize=0x{:X} afterBlock=[0x{:X},0x{:X}) afterPhysical=0x{:X}",
                    diagnosticEvents - count + i, event.kind, event.address, event.size, event.minSize, event.revisionBefore, event.revisionAfter,
                    event.before.viewValid, event.before.viewSize, event.before.blockStart, event.before.blockEnd, event.before.physical,
                    event.after.viewValid, event.after.viewSize, event.after.blockStart, event.after.blockEnd, event.after.physical);
            }
            const auto revision{gmmu.MappingRevision()};
            u64 address{selector.address}, remaining{std::max<u64>(selector.size, 1)};
            const auto requested{remaining}; // Size-zero binds probe one address; no guest byte is read.
            for (size_t i{}; i < 8 && remaining && address < gmmu.vaLimit; ++i) {
                auto identity{gmmu.InspectMappingIdentity(address)};
                auto [mapping, offset]{gmmu.LookupBlock(address)};
                const auto available{offset < mapping.size() ? mapping.size() - offset : 0};
                LOGE("CBUF_DIAG mapping piece={} address=0x{:X} resident={} physicalBase=0x{:X} blockBytes=0x{:X} offset=0x{:X} available=0x{:X} identityRevision={} generation={} identity=[0x{:X},0x{:X})",
                    i, address, mapping.valid(), reinterpret_cast<uintptr_t>(mapping.data()), mapping.size(), offset, available,
                    identity.revision, identity.generation, identity.begin, identity.end);
                if (!available || available > gmmu.vaLimit - address) break;
                const auto step{std::min<u64>(remaining, available)};
                address += step; remaining -= step;
            }
            LOGE("CBUF_DIAG probe requested=0x{:X} remaining=0x{:X} revisions={}/{}; generation=0 means untracked, null means nonresident/sparse, remaining may be bounded/truncated",
                requested, remaining, revision, gmmu.MappingRevision());
        } catch (...) {} // Diagnosis must never replace the original exception.
    }

    ConstantBuffers::ConstantBuffers(DirtyManager &manager, const ConstantBufferSelectorState::EngineRegisters &constantBufferSelectorRegisters) : selectorState{manager, constantBufferSelectorRegisters} {}

    void ConstantBuffers::MarkAllDirty() {
        selectorState.MarkDirty(true);
    }

    void ConstantBuffers::Load(InterconnectContext &ctx, span<u32> data, u32 offset) {
        TRACE_EVENT("gpu", "ConstantBuffers::Load");
        // The contents of an already-bound CBUF can change without a new bind.
        // Its descriptor must be refreshed rather than reused unchanged.
        DisableQuickBind();

        auto &view{*selectorState.UpdateGet(ctx, data.size_bytes()).view};
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
        auto &selector{selectorState.UpdateGet(ctx)};
        auto &view{*selector.view};
        if (!view) {
            selector.DiagnoseBindFailure(ctx, stage, index);
            throw exception("Constant buffer selector is not mapped");
        }

        boundConstantBuffers[static_cast<size_t>(stage)][index] = {view};

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
