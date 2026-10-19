// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <limits>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool /*splitMappingWarn*/) {
        if (!size) {
            PurgeCaches();
            return;
        }

        auto &gmmu{ctx.channelCtx.asCtx->gmmu};
        const auto generation{gmmu.GetMappingGeneration()};
        if (mappingAddressSpace != &gmmu || mappingGeneration != generation ||
            address < blockMappingStartAddr || address >= blockMappingEndAddr) {
            mappingAddressSpace = &gmmu;
            mappingGeneration = generation;
            u64 blockOffset{};
            std::tie(blockMapping, blockOffset) = gmmu.LookupBlock(address);
            blockMappingStartAddr = address - blockOffset;
            blockMappingEndAddr = blockMappingStartAddr + blockMapping.size();
            view = {};
        }

        const auto attachBuffer{[&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
            ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
        }};
        if (blockMapping.valid() && size <= blockMappingEndAddr - address) {
            auto viewMapping{blockMapping.subspan(address - blockMappingStartAddr, size)};
            if (view) {
                auto previous{view.GetBuffer()->TryGetView(viewMapping)};
                if (previous) {
                    ctx.gpu.buffer.PrepareBufferAccess(*previous.GetBuffer(), false, previous.GetOffset(), size);
                    view = previous;
                    return;
                }
            }
            view = ctx.gpu.buffer.FindOrCreate(viewMapping, ctx.executor.tag, attachBuffer);
            return;
        }

        // Vulkan sparse binding is not universally available. Use a staged logical
        // buffer with a zero-copy CPU mirror of each physical span instead. Include
        // page padding so offsets retain the guest's Vulkan alignment guarantees.
        const auto alignedAddress{util::AlignDown(address, static_cast<u64>(constant::PageSize))};
        if (size > std::numeric_limits<u64>::max() - address ||
            address + size > std::numeric_limits<u64>::max() - (constant::PageSize - 1))
            throw exception("GPU buffer range overflows its address space");
        const auto alignedEnd{util::AlignUp(address + size, static_cast<u64>(constant::PageSize))};
        const auto translations{gmmu.TranslateMappings(alignedAddress, alignedEnd - alignedAddress)};
        std::vector<span<u8>> mappings;
        mappings.reserve(translations.size());
        for (const auto &translation : translations) {
            if (translation.kind == MemoryMappingKind::Unmapped) {
                view = {};
                return;
            }
            mappings.push_back(translation.memory);
        }
        view = ctx.gpu.buffer.FindOrCreateDisjoint(mappings, address - alignedAddress, size,
                                                  ctx.executor.tag, attachBuffer);
    }

    void CachedMappedBufferView::PurgeCaches() {
        view = {};
        blockMappingEndAddr = 0; // Will force a retranslate of `blockMapping` on the next `Update()` call
    }

    void ConstantBuffer::Read(CommandExecutor &executor, span<u8> dstBuffer, size_t srcOffset, std::source_location location) {
        if (!view || srcOffset > view.size || dstBuffer.size() > view.size - srcOffset)
            throw exception("Constant buffer read exceeds its mapped view");
        ContextLock lock{executor.tag, view};
        view.Read(lock.IsFirstUsage(), [location, srcOffset, size = dstBuffer.size()] {
            // TODO: here we should trigger `Execute()`, however that doesn't currently work due to Read being called mid-draw and attached objects not handling this case
            LOGW("GPU dirty buffer reads for attached buffers are unimplemented (caller: {}:{}, function: {}, offset: 0x{:X}, size: 0x{:X})",
                 location.file_name(), location.line(), location.function_name(), srcOffset, size);
        }, dstBuffer, srcOffset);
    }
}
