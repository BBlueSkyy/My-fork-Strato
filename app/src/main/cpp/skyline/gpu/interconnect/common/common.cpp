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

        // Frequent split SSBOs must not retranslate and allocate a small vector
        // on every draw. A cache hit is valid until PurgeCaches() invalidates the
        // underlying GPU virtual mappings. Both successful and legacy fallback
        // views are cached only for their exact requested range.
        if (splitMappingWarn && lastSplitResolved && view &&
            address == lastSplitAddress && size == lastSplitSize) {
            return;
        }
        lastSplitResolved = false;

        auto fullMapping{blockMapping.subspan(address - blockMappingStartAddr)};
        const bool isSplit{fullMapping.size() < size};

        // Some callers supply an upper bound rather than a guaranteed mapped
        // length. Preserve their original clamping behavior when warned=false.
        if (splitMappingWarn && isSplit) {
            bool resolved{};
            size_t mappingCount{};
            size_t secondMappingSize{};
            const char *failureReason{"gpu-address-overflow"};
            if (size <= std::numeric_limits<u64>::max() - address) {
                auto mappings{ctx.channelCtx.asCtx->gmmu.TranslateRange(address, size)};
                mappingCount = mappings.size();
                secondMappingSize = mappingCount > 1 ? mappings[1].size() : 0;
                failureReason = "incomplete-or-empty-range";

                // Only a fully mapped, physically adjacent sequence can use the
                // existing BufferManager and its zero-copy BufferView semantics.
                // Noncontiguous/sparse/aliased mappings require a separate
                // multi-mapping implementation and must not be misrepresented
                // by a single span.
                u64 translatedSize{};
                uintptr_t nextPhysical{};
                bool physicallyContiguous{!mappings.empty()};
                for (const auto &mapping : mappings) {
                    if (!mapping.valid() || mapping.empty()) {
                        failureReason = "unmapped-or-empty-piece";
                        physicallyContiguous = false;
                        break;
                    }
                    if (mapping.size() > size - translatedSize) {
                        failureReason = "translated-range-exceeds-request";
                        physicallyContiguous = false;
                        break;
                    }
                    const uintptr_t begin{reinterpret_cast<uintptr_t>(mapping.data())};
                    if (begin == 0 ||
                        mapping.size() > std::numeric_limits<uintptr_t>::max() - begin) {
                        failureReason = "physical-address-overflow";
                        physicallyContiguous = false;
                        break;
                    }
                    if (translatedSize != 0 && begin != nextPhysical) {
                        failureReason = "noncontiguous-physical-pieces";
                        physicallyContiguous = false;
                        break;
                    }
                    translatedSize += mapping.size();
                    nextPhysical = begin + mapping.size();
                }
                if (physicallyContiguous && translatedSize == size &&
                    mappings.front().data() == fullMapping.data()) {
                    failureReason = "buffer-view-not-created";
                    auto completeMapping{span<u8>{mappings.front().data(), size}};
                    if (view) {
                        if (auto reused{view.GetBuffer()->TryGetView(completeMapping)}; reused) {
                            view = reused;
                            resolved = true;
                        }
                    }
                    if (!resolved) {
                        view = ctx.gpu.buffer.FindOrCreate(
                            completeMapping, ctx.executor.tag,
                            [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                                ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
                            });
                        resolved = static_cast<bool>(view);
                    }
                } else if (physicallyContiguous && translatedSize == size) {
                    failureReason = "first-physical-mapping-mismatch";
                }
            }

            if (resolved) {
                // Emit only cold-path successes, not repeated draw-time cache hits.
                static std::atomic<u64> contiguousRecoveryCount{};
                const u64 recovery{contiguousRecoveryCount.fetch_add(1, std::memory_order_relaxed) + 1};
                if (recovery <= 8 || (recovery & 0x3FF) == 0) {
                    LOGI("Split buffer mapping recovered: count={} address=0x{:X} size=0x{:X} mappings={}",
                         recovery, address, size, mappingCount);
                }
                lastSplitAddress = address;
                lastSplitSize = size;
                lastSplitResolved = true;
                return;
            }

            // Bounded diagnosis: unlimited warning spam previously caused extra
            // CPU work. Preserve the failure instead of pretending that the
            // missing portion was mapped.
            static std::atomic<u64> warningCount{};
            const u64 hit{warningCount.fetch_add(1, std::memory_order_relaxed) + 1};
            if (hit <= 16 || (hit & 0x3FF) == 0) {
                LOGW("Split buffer mappings are not supported: hit={} address=0x{:X} size=0x{:X} first=0x{:X} mappings={} second=0x{:X} reason={} caller={}:{} function={}",
                     hit, address, size, fullMapping.size(), mappingCount, secondMappingSize,
                     failureReason, location.file_name(), location.line(), location.function_name());
            }
        }

        // Legacy behavior for genuinely non-contiguous mappings: do not bind
        // out-of-range memory or create a writable GPU staging alias.
        auto viewMapping{fullMapping.first(std::min(fullMapping.size(), size))};
        if (view) {
            if (view = view.GetBuffer()->TryGetView(viewMapping); view) {
                if (splitMappingWarn && isSplit) {
                    lastSplitAddress = address;
                    lastSplitSize = size;
                    lastSplitResolved = true;
                }
                return;
            }
        }

        view = ctx.gpu.buffer.FindOrCreate(
            viewMapping, ctx.executor.tag,
            [&ctx](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                ctx.executor.AttachLockedBuffer(buffer, std::move(lock));
            });
        if (splitMappingWarn && isSplit && view) {
            lastSplitAddress = address;
            lastSplitSize = size;
            lastSplitResolved = true;
        }
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
