// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include "megabuffer.h"

namespace skyline::gpu {
    MegaBufferChunk::MegaBufferChunk(GPU &gpu) : backing{gpu.memory.AllocateBuffer(MegaBufferChunkSize)}, freeRegion{backing.subspan(PAGE_SIZE)} {}

    bool MegaBufferChunk::TryReset() {
        if (cycle && cycle->Poll(true)) {
            freeRegion = backing.subspan(PAGE_SIZE);
            cycle = nullptr;
            return true;
        }

        return cycle == nullptr;
    }

    vk::Buffer MegaBufferChunk::GetBacking() const {
        return backing.vkBuffer;
    }

    std::pair<vk::DeviceSize, span<u8>> MegaBufferChunk::Allocate(const std::shared_ptr<FenceCycle> &newCycle, vk::DeviceSize size, bool pageAlign) {
        if (pageAlign) {
            // If page aligned data was requested then align the free
            auto alignedFreeBase{util::AlignUp(static_cast<size_t>(freeRegion.data() - backing.data()), PAGE_SIZE)};
            freeRegion = backing.subspan(alignedFreeBase);
        }

        if (size > freeRegion.size())
            return {0, {}};

        if (cycle != newCycle) {
            newCycle->ChainCycle(cycle);
            cycle = newCycle;
        }

        // Allocate space for data from the free region
        auto resultSpan{freeRegion.subspan(0, size)};

        // Move the free region along
        freeRegion = freeRegion.subspan(size);

        return {static_cast<vk::DeviceSize>(resultSpan.data() - backing.data()), resultSpan};
    }

    MegaBufferAllocator::MegaBufferAllocator(GPU &gpu) : gpu{gpu}, activeChunk{chunks.emplace(chunks.end(), gpu)}, collectionCursor{activeChunk} {}

    MegaBufferAllocator::Diagnostics MegaBufferAllocator::GetDiagnostics() const noexcept {
        return {
            .rollovers = rollover.load(std::memory_order_relaxed),
            .allocatedChunks = allocatedChunks.load(std::memory_order_relaxed),
            .peakChunks = peakChunks.load(std::memory_order_relaxed),
            .poolChunks = poolChunks.load(std::memory_order_relaxed),
            .candidatesInspected = candidatesInspected.load(std::memory_order_relaxed),
            .candidatesPendingFence = candidatesPendingFence.load(std::memory_order_relaxed),
            .retirementsQueued = retirementsQueued.load(std::memory_order_relaxed),
            .retirementAllocationFailures = retirementAllocationFailures.load(std::memory_order_relaxed),
        };
    }

    void MegaBufferAllocator::CollectIdleChunk(const std::shared_ptr<FenceCycle> &cycle) {
        if (chunks.size() <= RetainedChunkCount || !retiredChunk.expired() || cycle->Poll())
            return;

        if (collectionCursor == chunks.end())
            collectionCursor = chunks.begin();
        auto candidate{collectionCursor++};
        candidatesInspected.fetch_add(1, std::memory_order_relaxed);
        if (candidate == activeChunk || rollover.load(std::memory_order_relaxed) - candidate->lastUseRollover < IdleRolloverCount)
            return;
        if (!candidate->TryReset()) {
            candidatesPendingFence.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        try {
            // Attach before splicing: allocation failure leaves the node in its pool.
            // The current cycle is still being recorded; the existing cycle waiter
            // destroys this completed surplus off the allocation thread (cancellation
            // can release it immediately). The weak reference bounds queued garbage
            // even if that submission stalls.
            auto retired{std::make_shared<decltype(chunks)>()};
            cycle->AttachObject(retired);
            retired->splice(retired->end(), chunks, candidate);
            retiredChunk = retired;
            poolChunks.fetch_sub(1, std::memory_order_relaxed);
            retirementsQueued.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::bad_alloc &) {
            retirementAllocationFailures.fetch_add(1, std::memory_order_relaxed);
            // Collection is optional; preserve usable pool capacity on host OOM.
        }
    }

    MegaBufferAllocator::Allocation MegaBufferAllocator::Allocate(const std::shared_ptr<FenceCycle> &cycle, vk::DeviceSize size, bool pageAlign) {
        if (auto allocation{activeChunk->Allocate(cycle, size, pageAlign)}; allocation.first)
            return {activeChunk->GetBacking(), allocation.first, allocation.second};

        activeChunk->lastUseRollover = ++rollover;
        activeChunk = ranges::find_if(chunks, [&](auto &chunk) { return chunk.TryReset(); });
        if (activeChunk == chunks.end()) { // If there are no chunks available, allocate a new one
            activeChunk = chunks.emplace(chunks.end(), gpu);
            allocatedChunks.fetch_add(1, std::memory_order_relaxed);
            const auto count{poolChunks.fetch_add(1, std::memory_order_relaxed) + 1};
            peakChunks.store(std::max(peakChunks.load(std::memory_order_relaxed), count), std::memory_order_relaxed);
        }
        activeChunk->lastUseRollover = rollover;

        if (auto allocation{activeChunk->Allocate(cycle, size, pageAlign)}; allocation.first) {
            CollectIdleChunk(cycle);
            // Emit a sparse progress sample during gameplay, since Android may
            // terminate the session without reaching the normal shutdown summary.
            // No extra fence polling, pool traversal or logging on fast allocations.
            const auto currentRollover{rollover.load(std::memory_order_relaxed)};
            if (currentRollover == 1 || (currentRollover % 256) == 0) {
                const auto gc{GetDiagnostics()};
                LOGINF("[GPU-GC] progress rollovers={} allocated_chunks={} peak_chunks={} pool_chunks={} "
                       "candidates_inspected={} pending_fence={} retirements_queued={} "
                       "pool_capacity_mib={} (queued retirements are not confirmed driver frees)",
                       gc.rollovers, gc.allocatedChunks, gc.peakChunks, gc.poolChunks,
                       gc.candidatesInspected, gc.candidatesPendingFence, gc.retirementsQueued,
                       gc.poolChunks * (MegaBufferChunkSize / (1024 * 1024)));
            }
            return {activeChunk->GetBacking(), allocation.first, allocation.second};
        } else
            throw exception("Failed to to allocate megabuffer space for size: 0x{:X}", size);
    }

    MegaBufferAllocator::Allocation MegaBufferAllocator::Push(const std::shared_ptr<FenceCycle> &cycle, span<u8> data, bool pageAlign) {
        auto allocation{Allocate(cycle, data.size(), pageAlign)};
        allocation.region.copy_from(data);
        return allocation;
    }
}
