// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn) {
        if (!size) {
            view = {};
            return;
        }

        if (address < blockMappingStartAddr || address >= blockMappingEndAddr) {
            u64 blockOffset{};
            std::tie(blockMapping, blockOffset) = ctx.channelCtx.asCtx->gmmu.LookupBlock(address);
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

        if (fullMapping.size() < size) {
            // Separate GMMU blocks can still point at physically contiguous guest
            // memory. TranslateRange coalesces those blocks into one span, which is
            // fully compatible with the existing BufferManager representation.
            auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(address, size)};
            if (mappings.size() == 1 && mappings.front().valid() && mappings.front().size() >= size) {
                blockMapping = mappings.front();
                blockMappingStartAddr = address;
                blockMappingEndAddr = address + size;
                fullMapping = blockMapping;
            } else if (splitMappingWarn) {
                LOGW("Split buffer mapping is physically non-contiguous: address 0x{:X}, requested 0x{:X}, contiguous 0x{:X}, mappings {}",
                     address, size, fullMapping.size(), mappings.size());
            }
        }

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
