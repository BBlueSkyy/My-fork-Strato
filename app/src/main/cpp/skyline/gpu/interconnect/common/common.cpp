// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn, std::source_location location) {
        // Ignore size for the mapping end check here as we don't support buffers split across multiple mappings so only the first one would be used anyway. It's also impossible for the mapping to have been remapped with a larger one since the original lookup because the we force the mapping to be reset after semaphores
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
            auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(address, size)};

            bool physicallyContiguous{!mappings.empty()};
            size_t translatedSize{};
            u8 *expectedNext{};

            for (auto mapping : mappings) {
                if (!mapping.valid() || mapping.empty() || (expectedNext && mapping.data() != expectedNext)) {
                    physicallyContiguous = false;
                    break;
                }

                translatedSize += mapping.size();
                expectedNext = mapping.data() + mapping.size();
            }

            if (physicallyContiguous && translatedSize >= size) {
                auto viewMapping{span<u8>{mappings.front().data(), size}};

                if (view)
                    if (view = view.GetBuffer()->TryGetView(viewMapping); view)
                        return;

                view = ctx.gpu.buffer.FindOrCreate(viewMapping, ctx.executor.tag, [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                    ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
                });
                return;
            }

            if (splitMappingWarn) {
                static std::atomic<u64> splitMappingLogCount{};
                u64 count{splitMappingLogCount.fetch_add(1, std::memory_order_relaxed) + 1};

                // Keep diagnostic builds usable even when a title hits this path tens of
                // thousands of times per second. Preserve the first samples, then only
                // emit periodic snapshots with the cumulative hit count.
                if (count <= 128 || (count & 0xFFF) == 0)
                    LOGW("Split buffer mappings are not supported (hit={}, address=0x{:X}, size=0x{:X}, first=0x{:X}, mappings={}, caller={}:{}, function={})",
                         count, address, size, fullMapping.size(), mappings.size(), location.file_name(), location.line(), location.function_name());
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
