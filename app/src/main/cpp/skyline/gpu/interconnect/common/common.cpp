// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <atomic>
#include <limits>
#include <gpu/buffer_manager.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn,
                                        std::source_location location) {
        // Keep the original O(1) single-block lookup. The GMMU cache is purged
        // after remapping/semaphores, exactly as before this change.
        if (address < blockMappingStartAddr || address >= blockMappingEndAddr) {
            u64 blockOffset{};
            std::tie(blockMapping, blockOffset) = ctx.channelCtx.asCtx->gmmu.LookupBlock(address);
            lastSplitResolved = false;
            if (!blockMapping.valid()) {
                view = {};
                blockMappingEndAddr = 0;
                return;
            }

            blockMappingStartAddr = address - blockOffset;
            blockMappingEndAddr = blockMappingStartAddr + blockMapping.size();
        }

        // Reuse an exact logical view until the normal GMMU/semaphore purge.
        if (splitMappingWarn && lastSplitResolved && view &&
            address == lastSplitAddress && size == lastSplitSize)
            return;
        lastSplitResolved = false;

        auto fullMapping{blockMapping.subspan(address - blockMappingStartAddr)};
        if (splitMappingWarn && fullMapping.size() < size) {
            // Mirror page-aligned physical pieces in GPU-virtual order. This is
            // one canonical Buffer, not a per-draw gather/scatter allocation.
            constexpr u64 pageMask{constant::PageSize - 1};
            if (address > std::numeric_limits<u64>::max() - pageMask ||
                size > std::numeric_limits<u64>::max() - pageMask - address) {
                view = {};
                return;
            }
            const u64 alignedAddress{util::AlignDown(address, constant::PageSize)};
            const u64 alignedEnd{util::AlignUp(address + size, constant::PageSize)};
            auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(alignedAddress, alignedEnd - alignedAddress)};
            GuestBuffer guest{GuestBuffer::Mappings{mappings.begin(), mappings.end()}};
            auto attach{[&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
            }};
            const char *failureReason{"unmapped-or-aliased-range"};
            if (guest.valid() && guest.size() == alignedEnd - alignedAddress) {
                failureReason = "incompatible-buffer-alias-layout";
                if (guest.mappings.size() == 1) {
                    // Physically adjacent GMMU blocks retain the ordinary path.
                    auto requested{guest.mappings.front().subspan(address - alignedAddress, size)};
                    view = ctx.gpu.buffer.FindOrCreate(requested, ctx.executor.tag, attach);
                } else {
                    view = ctx.gpu.buffer.FindOrCreate(guest, address - alignedAddress, size,
                                                       ctx.executor.tag, attach);
                }
                if (view) {
                    lastSplitAddress = address;
                    lastSplitSize = size;
                    lastSplitResolved = true;
                    return;
                }
            }

            // Never turn an exact writable descriptor into a first-piece view.
            // Missing pages or non-affine aliases cannot be guessed safely.
            view = {};
            static std::atomic<u64> warningCount{};
            const u64 hit{warningCount.fetch_add(1, std::memory_order_relaxed) + 1};
            if (hit <= 16 || (hit & 0x3FF) == 0)
                LOGW("Unresolved split buffer: hit={} address=0x{:X} size=0x{:X} mappings={} reason={} caller={}:{}",
                     hit, address, size, guest.mappings.size(), failureReason,
                     location.file_name(), location.line());
            return;
        }

        // Upper-bound callers (warn=false) retain the legacy clamp.
        auto viewMapping{fullMapping.first(std::min(fullMapping.size(), size))};
        if (view)
            if (view = view.GetBuffer()->TryGetView(viewMapping); view)
                return;
        view = ctx.gpu.buffer.FindOrCreate(viewMapping, ctx.executor.tag,
            [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
            });
    }

    void CachedMappedBufferView::PurgeCaches() {
        view = {};
        lastSplitResolved = false;
        blockMappingEndAddr = 0;
    }

    void ConstantBuffer::Read(CommandExecutor &executor, span<u8> dstBuffer, size_t srcOffset,
                              std::source_location location, bool retryPendingGpuWrites) {
        ContextLock lock{executor.tag, view};
        view.Read(lock.IsFirstUsage(), [retryPendingGpuWrites, location, srcOffset, size = dstBuffer.size()] {
            if (retryPendingGpuWrites) {
                // The caller is preparing a draw, possibly from the shader
                // compiler. A direct Submit here would run while the buffer
                // and other draw resources are locked, and would invalidate
                // partially constructed pipeline/descriptor state. Unwind and
                // let the outer draw retry boundary submit before recreating it.
                throw GpuDirtyConstantBufferReadRequest{};
            }
            LOGW("GPU dirty buffer reads for attached buffers are unimplemented (caller: {}:{}, function: {}, offset: 0x{:X}, size: 0x{:X})",
                 location.file_name(), location.line(), location.function_name(), srcOffset, size);
        }, dstBuffer, srcOffset);
    }
}
