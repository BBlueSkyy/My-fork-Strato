// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstring>
#include <gpu/buffer_manager.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn, BufferMappingAccess access) {
        auto clearView{[&] {
            view = {};
            stagingBuffer.reset();
        }};

        if (!size) {
            clearView();
            return;
        }

        // The cached block is only reused when it contains the complete requested range. A
        // mapping assembled across GMMU blocks must never be cached as a single block because
        // one of its component mappings may be replaced independently.
        if (address < blockMappingStartAddr || address >= blockMappingEndAddr) {
            u64 blockOffset{};
            std::tie(blockMapping, blockOffset) = ctx.channelCtx.asCtx->gmmu.LookupBlock(address);
            if (!blockMapping.valid()) {
                blockMappingEndAddr = 0;
            } else {
                blockMappingStartAddr = address - blockOffset;
                blockMappingEndAddr = blockMappingStartAddr + blockMapping.size();
            }
        }

        span<u8> fullMapping{};
        if (blockMapping.valid() && address >= blockMappingStartAddr && address < blockMappingEndAddr)
            fullMapping = blockMapping.subspan(address - blockMappingStartAddr);

        auto useContiguousMapping{[&](span<u8> mapping) {
            bool previousViewWasStaged{static_cast<bool>(stagingBuffer)};
            stagingBuffer.reset();

            // First attempt to skip lookup by trying to reuse the previous view's underlying buffer.
            if (!previousViewWasStaged && view)
                if (view = view.GetBuffer()->TryGetView(mapping); view)
                    return;

            view = ctx.gpu.buffer.FindOrCreate(mapping, ctx.executor.tag, [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
            });
        }};

        if (fullMapping.valid() && fullMapping.size() >= size) {
            useContiguousMapping(fullMapping.first(size));
            return;
        }

        auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(address, size)};
        size_t translatedSize{};
        size_t mappedSize{};
        bool allMappingsValid{!mappings.empty()};
        for (auto mapping : mappings) {
            translatedSize += mapping.size();
            if (mapping.valid())
                mappedSize += mapping.size();
            else
                allMappingsValid = false;
        }
        bool rangeCovered{!mappings.empty() && translatedSize == size};
        bool fullyMapped{rangeCovered && allMappingsValid};

        // Multiple GMMU blocks may still resolve to one contiguous CPU range. Use it for this
        // lookup, but deliberately do not store it in the block cache.
        if (fullyMapped && mappings.size() == 1 && fullMapping.valid()) {
            useContiguousMapping(mappings.front());
            return;
        }

        if (rangeCovered && mappedSize && access == BufferMappingAccess::ReadOnly) {
            if (fullyMapped) {
                static std::atomic_flag splitMappingLogged{};
                if (!splitMappingLogged.test_and_set(std::memory_order_relaxed))
                    LOGI("Split read-only buffer mapping support active (first range: address 0x{:X}, size 0x{:X}, mappings {})",
                         address, size, mappings.size());
            } else {
                static std::atomic_flag partialMappingLogged{};
                if (!partialMappingLogged.test_and_set(std::memory_order_relaxed))
                    LOGI("Partially mapped read-only buffer staging active (first range: address 0x{:X}, size 0x{:X}, mapped 0x{:X}, mappings {})",
                         address, size, mappedSize, mappings.size());
            }

            auto newStagingBuffer{ctx.gpu.buffer.CreateHostOnlyBuffer(size)};
            ContextLock lock{ctx.executor.tag, *newStagingBuffer};
            auto stagingMapping{newStagingBuffer->GetBackingSpan()};

            // Preserve GPU virtual ordering while representing sparse/unmapped holes as zeroes.
            // This path is read-only, so no scatter/writeback is required after execution.
            size_t stagingOffset{};
            for (auto mapping : mappings) {
                auto *destination{stagingMapping.data() + stagingOffset};
                if (mapping.valid())
                    std::memcpy(destination, mapping.data(), mapping.size());
                else
                    std::memset(destination, 0, mapping.size());
                stagingOffset += mapping.size();
            }

            newStagingBuffer->BlockSequencedCpuBackingWrites();
            stagingBuffer = std::move(newStagingBuffer);
            view = stagingBuffer->GetView(0, size);
            ctx.executor.AttachLockedBuffer(stagingBuffer, std::move(lock));
            return;
        }

        if (splitMappingWarn) {
            static std::atomic_flag unsupportedReadOnlyLogged{};
            static std::atomic_flag unsupportedReadWriteLogged{};
            auto *logged{access == BufferMappingAccess::ReadOnly ? &unsupportedReadOnlyLogged : &unsupportedReadWriteLogged};
            if (!logged->test_and_set(std::memory_order_relaxed))
                LOGW("Unsupported {} buffer mapping: address 0x{:X}, requested 0x{:X}, contiguous 0x{:X}, mapped 0x{:X}, mappings {}",
                     access == BufferMappingAccess::ReadOnly ? "read-only" : "read-write",
                     address, size, fullMapping.size(), mappedSize, mappings.size());
        }

        if (fullMapping.valid())
            useContiguousMapping(fullMapping.first(std::min(fullMapping.size(), size)));
        else
            clearView();
    }

    void CachedMappedBufferView::PurgeCaches() {
        view = {};
        stagingBuffer.reset();
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
