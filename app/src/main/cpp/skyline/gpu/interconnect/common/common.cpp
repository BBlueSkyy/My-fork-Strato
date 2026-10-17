// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn) {
        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        const auto generation{gmmu.GetMappingGeneration()};
        // A semaphore flush still purges pipeline caches. Also retranslate this
        // local cache when nvhost replaces the same IOVA between updates.
        if (mappingAddressSpace != &gmmu || mappingGeneration != generation ||
            address < blockMappingStartAddr || address >= blockMappingEndAddr) {
            mappingAddressSpace = &gmmu;
            mappingGeneration = generation;
            u64 blockOffset{};
            std::tie(blockMapping, blockOffset) = gmmu.LookupBlock(address);
            if (!blockMapping.valid()) {
                view = {};
                blockMappingEndAddr = 0;
                return;
            }

            blockMappingStartAddr = address - blockOffset;
            blockMappingEndAddr = blockMappingStartAddr + blockMapping.size();
        }

        // Mapping from the start of the buffer view to the end of the block
        auto fullMapping{blockMapping.subspan(address - blockMappingStartAddr)};

        if (splitMappingWarn && fullMapping.size() < size)
            LOGW("GPU buffer crosses disjoint or nonresident mappings (requested: 0x{:X}, resident prefix: 0x{:X})", size, fullMapping.size());

        // Mapping covering just the requested input view (or less in the case of split mappings)
        auto viewMapping{fullMapping.first(std::min(fullMapping.size(), size))};

        // First attempt to skip lookup by trying to reuse the previous view's underlying buffer
        if (view)
            if (view = view.GetBuffer()->TryGetView(viewMapping); view)
                return;

        // Otherwise perform a full lookup
        view = ctx.gpu.buffer.FindOrCreate(viewMapping, ctx.executor.tag, [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
            ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
        });
    }

    void CachedMappedBufferView::PurgeCaches() {
        view = {};
        blockMappingEndAddr = 0; // Will force a retranslate of `blockMapping` on the next `Update()` call
    }

    void ConstantBuffer::Read(CommandExecutor &executor, span<u8> dstBuffer, size_t srcOffset, std::source_location location) {
        ContextLock lock{executor.tag, view};
        view.Read(lock.IsFirstUsage(), [location, srcOffset, size = dstBuffer.size()] {
            // TODO: here we should trigger `Execute()`, however that doesn't currently work due to Read being called mid-draw and attached objects not handling this case
            LOGW("GPU dirty buffer reads for attached buffers are unimplemented (caller: {}:{}, function: {}, offset: 0x{:X}, size: 0x{:X})",
                 location.file_name(), location.line(), location.function_name(), srcOffset, size);
        }, dstBuffer, srcOffset);
    }
}
