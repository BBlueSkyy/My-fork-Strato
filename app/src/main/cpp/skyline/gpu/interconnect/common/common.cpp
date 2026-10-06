// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include <atomic>
#include <limits>
#include "common.h"

namespace skyline::gpu::interconnect {
    void CachedMappedBufferView::Update(InterconnectContext &ctx, u64 address, u64 size, bool splitMappingWarn) {
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

        if (fullMapping.size() < size && splitMappingWarn) {
            if (size > std::numeric_limits<u64>::max() - address) {
                view = {};
                return;
            }

            const u64 alignedAddress{util::AlignDown(address, constant::PageSize)};
            const u64 alignedEnd{util::AlignUp(address + size, constant::PageSize)};
            auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(alignedAddress, alignedEnd - alignedAddress)};

            GuestBuffer::Mappings guestMappings{mappings.begin(), mappings.end()};
            GuestBuffer guest{std::move(guestMappings)};

            const char *failureReason{};
            if (guest.valid()) {
                auto splitView{ctx.gpu.buffer.FindOrCreate(guest, address - alignedAddress, size, ctx.executor.tag,
                                                          [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                                                              ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
                                                          })};
                if (splitView) {
                    view = splitView;
                    return;
                }

                failureReason = "buffer-manager-overlap";
            } else {
                bool hasInvalidMapping{};
                bool hasPhysicalOverlap{};
                for (size_t i{}; i < guest.mappings.size(); ++i) {
                    const auto &mapping{guest.mappings[i]};
                    if (!mapping.data() || !mapping.size()) {
                        hasInvalidMapping = true;
                        continue;
                    }

                    const auto begin{reinterpret_cast<uintptr_t>(mapping.data())};
                    const auto end{begin + mapping.size()};
                    for (size_t j{}; j < i; ++j) {
                        const auto &previous{guest.mappings[j]};
                        if (!previous.data() || !previous.size())
                            continue;

                        const auto previousBegin{reinterpret_cast<uintptr_t>(previous.data())};
                        const auto previousEnd{previousBegin + previous.size()};
                        if (begin < previousEnd && previousBegin < end) {
                            hasPhysicalOverlap = true;
                            break;
                        }
                    }
                }

                failureReason = hasInvalidMapping ? "unmapped-gap" :
                                hasPhysicalOverlap ? "physical-alias" :
                                "guest-validation";
            }

            static std::atomic_size_t splitFailureLogs{};
            auto failureIndex{splitFailureLogs.fetch_add(1, std::memory_order_relaxed)};
            if (failureIndex < 8) {
                LOGW("Split buffer mapping diagnostic: reason={}, gpu=0x{:X}, size=0x{:X}, aligned=0x{:X}-0x{:X}, mappings={}",
                     failureReason, address, size, alignedAddress, alignedEnd, guest.mappings.size());

                for (size_t i{}; i < std::min<size_t>(guest.mappings.size(), 4); ++i) {
                    const auto &mapping{guest.mappings[i]};
                    LOGW("Split buffer mapping diagnostic: mapping[{}]=0x{:X}+0x{:X}",
                         i, reinterpret_cast<uintptr_t>(mapping.data()), mapping.size());
                }

                LOGW("Split buffer mappings could not be resolved safely, using the first mapping");
            }
        }

        // Some callers intentionally supply an upper-bound size (for example the constant-buffer
        // selector's default 0x10000). Preserve the legacy clamp when splitMappingWarn is false.
        auto viewMapping{fullMapping.first(std::min(fullMapping.size(), size))};

        if (view)
            if (view = view.GetBuffer()->TryGetView(viewMapping); view)
                return;

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
