// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/settings.h>
#include <limits>
#include <gpu.h>
#include "buffer_manager.h"

namespace skyline::gpu {
    BufferManager::BufferManager(GPU &gpu) : gpu{gpu} {}

    void BufferMappingReconciliationRequest::Complete() const {
        manager->PrepareBufferAccess(*target, write, offset, size, allowRepeatedCpuWrite);
    }

    std::vector<std::shared_ptr<Buffer>> BufferManager::SnapshotAliases() {
        std::scoped_lock lock{aliasMutex};
        std::vector<std::shared_ptr<Buffer>> buffers;
        for (auto it{aliasBuffers.begin()}; it != aliasBuffers.end();) {
            if (auto buffer{it->lock()}) {
                buffers.push_back(std::move(buffer));
                ++it;
            } else {
                it = aliasBuffers.erase(it);
            }
        }
        return buffers;
    }

    static bool PhysicalRangesOverlap(GuestBuffer first, GuestBuffer second) {
        const auto a{reinterpret_cast<uintptr_t>(first.data())}, b{reinterpret_cast<uintptr_t>(second.data())};
        return first.valid() && second.valid() && (a <= b ? b - a < first.size() : a - b < second.size());
    }

    void BufferManager::PrepareBufferAccess(Buffer &target, bool write, vk::DeviceSize offset, vk::DeviceSize size, bool allowRepeatedCpuWrite) {
        if (!hasAggregates || !target.aliasValid)
            return;
        if (size == VK_WHOLE_SIZE)
            size = target.mirror.size() - offset;
        std::vector<GuestBuffer> targetMappings;
        target.VisitPhysicalRanges(offset, size, [&](GuestBuffer mapping) { targetMappings.push_back(mapping); });
        if (write && target.HasRepeatedPhysicalMappings()) {
            std::scoped_lock stateLock{target.stateMutex};
            if (!allowRepeatedCpuWrite || target.dirtyState == Buffer::DirtyState::GpuDirty ||
                target.SequencedCpuBackingWritesBlocked() || !target.PollFence())
                throw exception("Repeated physical buffer writes require an immediately writable ordered CPU transport");
        }

        // Read-only aliases may coexist. A write changes authority only for actual
        // resident intersections, and only queued conflicts require a submission.
        for (const auto &source : SnapshotAliases()) {
            if (source.get() == &target || !source->aliasValid)
                continue;
            bool overlaps{};
            for (const auto mapping : targetMappings)
                for (const auto other : source->guestMappings)
                    overlaps |= PhysicalRangesOverlap(mapping, other);
            if (!overlaps)
                continue;
            std::unique_lock stateLock{source->stateMutex};
            bool pendingOverlap{};
            for (const auto mapping : targetMappings)
                pendingOverlap |= source->aliasPendingWrites.Intersect(mapping);
            const bool sourceWrites{pendingOverlap || source->dirtyState == Buffer::DirtyState::GpuDirty || source->directGpuWritesActive};
            if (source->SequencedCpuBackingWritesBlocked() && (sourceWrites || write))
                throw BufferMappingReconciliationRequest{this, target.shared_from_this(), write, offset, size, allowRepeatedCpuWrite};
            if (!sourceWrites && !(write && source->isDirect && source->cycle))
                continue;
            // Use the raw recursive mutex: a temporary ContextLock with another
            // tag would reset a buffer still attached to the executor on unwind.
            stateLock.unlock();
            std::scoped_lock sourceLock{source->mutex};
            stateLock.lock();
            if (!source->aliasValid)
                continue;
            if (source->SequencedCpuBackingWritesBlocked())
                throw BufferMappingReconciliationRequest{this, target.shared_from_this(), write, offset, size, allowRepeatedCpuWrite};
            source->WaitOnFence();
            if (!sourceWrites)
                continue;
            if (source->isDirect)
                source->RefreshGpuWritesActiveDirect(true);
            else
                source->SynchronizeGuest();
            source->aliasPendingWrites.Clear();
            std::scoped_lock targetStateLock{target.stateMutex};
            if (!target.isDirect) {
                if (target.dirtyState == Buffer::DirtyState::GpuDirty)
                    throw exception("Conflicting GPU-dirty aliases cannot both own the same physical bytes");
                if (target.AllCpuBackingWritesBlocked())
                    throw BufferMappingReconciliationRequest{this, target.shared_from_this(), write, offset, size, allowRepeatedCpuWrite};
                target.dirtyState = Buffer::DirtyState::CpuDirty;
                target.AdvanceSequence();
            }
        }
    }

    void BufferManager::NotifyBufferWrite(Buffer &source, vk::DeviceSize offset, vk::DeviceSize size) {
        if (!source.aliasValid)
            return;
        source.TrackGuestUsage(source.aliasPendingWrites, offset, size);
        if (!hasAggregates)
            return;
        std::vector<GuestBuffer> written;
        source.VisitPhysicalRanges(offset, size, [&](GuestBuffer mapping) { written.push_back(mapping); });
        for (const auto &alias : SnapshotAliases()) {
            if (alias.get() == &source || !alias->aliasValid || alias->isDirect)
                continue;
            bool overlaps{};
            for (const auto mapping : written)
                for (const auto other : alias->guestMappings)
                    overlaps |= PhysicalRangesOverlap(mapping, other);
            if (!overlaps)
                continue;
            std::scoped_lock stateLock{alias->stateMutex};
            // PrepareBufferAccess established exclusive write authority first.
            if (alias->dirtyState != Buffer::DirtyState::GpuDirty) {
                alias->dirtyState = Buffer::DirtyState::CpuDirty;
                alias->AdvanceSequence();
            }
        }
    }

    BufferView BufferManager::FindOrCreateDisjoint(const std::vector<GuestBuffer> &mappings, vk::DeviceSize offset, vk::DeviceSize size,
                                                  ContextTag tag, const std::function<void(std::shared_ptr<Buffer>, ContextLock<Buffer> &&)> &/*attachBuffer*/) {
        if (mappings.empty())
            return {};
        size_t total{};
        for (const auto mapping : mappings) {
            if (!mapping.size() || mapping.size() % constant::PageSize ||
                (mapping.valid() && reinterpret_cast<uintptr_t>(mapping.data()) % constant::PageSize) ||
                mapping.size() > std::numeric_limits<size_t>::max() - total)
                throw exception("Invalid page-aligned aggregate buffer mapping");
            total += mapping.size();
        }
        if (offset > total || size > total - offset)
            throw exception("Aggregate buffer view exceeds its logical extent");
        for (const auto &buffer : aggregateBuffers) {
            const auto &existing{buffer->guestMappings};
            if (existing.size() == mappings.size() && std::equal(existing.begin(), existing.end(), mappings.begin(), [](GuestBuffer a, GuestBuffer b) {
                return a.data() == b.data() && a.size() == b.size();
            })) {
                ContextLock lock{tag, *buffer};
                PrepareBufferAccess(*buffer, false, offset, size);
                return buffer->GetView(offset, size);
            }
        }
        LockedBuffer buffer{std::make_shared<Buffer>(delegateAllocatorState, gpu, mappings, nextBufferId++), tag};
        buffer->SetupStagedTraps();
        aggregateBuffers.push_back(*buffer);
        {
            std::scoped_lock lock{aliasMutex};
            aliasBuffers.push_back(*buffer);
        }
        hasAggregates = true;
        PrepareBufferAccess(**buffer, false, offset, size);
        return buffer->GetView(offset, size);
    }

    bool BufferManager::BufferLessThan(const std::shared_ptr<Buffer> &it, u8 *pointer) {
        return it->guest->begin().base() < pointer;
    }

    BufferManager::LockedBuffer::LockedBuffer(std::shared_ptr<Buffer> pBuffer, ContextTag tag) : buffer{std::move(pBuffer)}, lock{tag, *buffer}, stateLock(buffer->stateMutex) {}

    Buffer *BufferManager::LockedBuffer::operator->() const {
        return buffer.get();
    }

    std::shared_ptr<Buffer> &BufferManager::LockedBuffer::operator*() {
        return buffer;
    }

    BufferManager::LockedBuffers BufferManager::Lookup(span<u8> range, ContextTag tag) {
        LockedBuffers overlaps;

        // Try to do a fast lookup in the page table
        auto lookupBuffer{bufferTable[range.begin().base()]};
        if (lookupBuffer != nullptr && lookupBuffer->guest->contains(range)) {
            overlaps.emplace_back(lookupBuffer->shared_from_this(), tag);
            return overlaps;
        }

        // If we cannot find the buffer quickly, do a binary search to find all overlapping buffers
        auto entryIt{std::lower_bound(bufferMappings.begin(), bufferMappings.end(), range.end().base(), BufferLessThan)};
        while (entryIt != bufferMappings.begin() && (*--entryIt)->guest->begin() <= range.end())
            if ((*entryIt)->guest->end() > range.begin())
                overlaps.emplace_back(*entryIt, tag);

        return overlaps;
    }

    void BufferManager::InsertBuffer(std::shared_ptr<Buffer> buffer) {
        {
            std::scoped_lock lock{aliasMutex};
            aliasBuffers.push_back(buffer);
        }
        auto bufferStart{buffer->guest->begin().base()}, bufferEnd{buffer->guest->end().base()};
        bufferTable.Set(bufferStart, bufferEnd, buffer.get());
        bufferMappings.insert(std::lower_bound(bufferMappings.begin(), bufferMappings.end(), bufferEnd, BufferLessThan), std::move(buffer));
    }

    void BufferManager::DeleteBuffer(const std::shared_ptr<Buffer> &buffer) {
        bufferTable.Set(buffer->guest->begin().base(), buffer->guest->end().base(), nullptr);
        bufferMappings.erase(std::find(bufferMappings.begin(), bufferMappings.end(), buffer));
    }

    BufferManager::LockedBuffer BufferManager::CoalesceBuffers(span<u8> range, const LockedBuffers &srcBuffers, ContextTag tag) {
        TRACE_EVENT("gpu", "BufferManager::CoalesceBuffers");
        std::shared_ptr<FenceCycle> newBufferCycle{};
        for (auto &srcBuffer : srcBuffers) {
            // Since new direct buffers will share the underlying backing of source buffers we don't need to wait for the GPU if they're dirty, for non direct buffers we do though as otherwise we won't be able to migrate their contents to the new backing
            if (!*gpu.state.settings->useDirectMemoryImport && (srcBuffer->dirtyState == Buffer::DirtyState::GpuDirty || srcBuffer->AllCpuBackingWritesBlocked()))
                srcBuffer->WaitOnFence();

            // We can't chain cycles here as that may also introduce a deadlock since we have no way to determine what order to chain them in right now
            // Wait on all source buffers before we lock the recreation mutex as locking it may prevent submissions of the cycles and introduce a deadlock
            if (newBufferCycle && srcBuffer->cycle != newBufferCycle)
                srcBuffer->WaitOnFence();
            else
                newBufferCycle = srcBuffer->cycle;
        }

        if (!range.valid())
            range = span<u8>{srcBuffers.front().buffer->guest->begin(), srcBuffers.back().buffer->guest->end()};

        auto lowestAddress{range.begin().base()}, highestAddress{range.end().base()};
        for (const auto &srcBuffer : srcBuffers) {
            // Find the extents of the new buffer we want to create that can hold all overlapping buffers
            auto mapping{*srcBuffer->guest};
            if (mapping.begin().base() < lowestAddress)
                lowestAddress = mapping.begin().base();
            if (mapping.end().base() > highestAddress)
                highestAddress = mapping.end().base();
        }

        LockedBuffer newBuffer{std::make_shared<Buffer>(delegateAllocatorState, gpu, span<u8>{lowestAddress, highestAddress}, nextBufferId++, *gpu.state.settings->useDirectMemoryImport), tag}; // If we don't lock the buffer prior to trapping it during synchronization, a race could occur with a guest trap acquiring the lock before we do and mutating the buffer prior to it being ready

        // Coherence can request an executor retry. Complete that preflight before
        // relinking stable delegates, and before holding the mutex needed by
        // recording workers while waiting on an alias's previous submission.
        PrepareBufferAccess(**newBuffer);
        std::scoped_lock lock{recreationMutex};
        newBuffer->SetupStagedTraps();
        newBuffer->SynchronizeHost(false); // Overlaps don't necessarily fully cover the buffer so we have to perform a sync here to prevent any gaps
        newBuffer->cycle = newBufferCycle;

        auto copyBuffer{[](auto dstGuest, auto srcGuest, auto dstPtr, auto srcPtr) {
            if (dstGuest.begin().base() <= srcGuest.begin().base()) {
                size_t dstOffset{static_cast<size_t>(srcGuest.begin().base() - dstGuest.begin().base())};
                size_t copySize{std::min(dstGuest.size() - dstOffset, srcGuest.size())};
                std::memcpy(dstPtr + dstOffset, srcPtr, copySize);
            } else if (dstGuest.begin().base() > srcGuest.begin().base()) {
                size_t srcOffset{static_cast<size_t>(dstGuest.begin().base() - srcGuest.begin().base())};
                size_t copySize{std::min(dstGuest.size(), srcGuest.size() - srcOffset)};
                std::memcpy(dstPtr, srcPtr + srcOffset, copySize);
            }
        }}; //!< Copies between two buffers based off of their mappings in guest memory

        for (auto &srcBuffer : srcBuffers) {
            // All newly created buffers that have this set are guaranteed to be attached in buffer FindOrCreate, attach will then lock the buffer without resetting this flag, which will only finally be reset when the lock is released
            if (newBuffer->backingImmutability == Buffer::BackingImmutability::None && srcBuffer->backingImmutability != Buffer::BackingImmutability::None)
                newBuffer->backingImmutability = srcBuffer->backingImmutability;
            else if (srcBuffer->backingImmutability == Buffer::BackingImmutability::AllWrites)
                newBuffer->backingImmutability = Buffer::BackingImmutability::AllWrites;

            newBuffer->everHadInlineUpdate |= srcBuffer->everHadInlineUpdate;
            newBuffer->aliasPendingWrites.Merge(srcBuffer->aliasPendingWrites);

            if (!*gpu.state.settings->useDirectMemoryImport) {
                if (srcBuffer->dirtyState == Buffer::DirtyState::GpuDirty) {
                    if (srcBuffer.lock.IsFirstUsage() && newBuffer->dirtyState != Buffer::DirtyState::GpuDirty)
                        copyBuffer(*newBuffer->guest, *srcBuffer->guest, newBuffer->mirror.data(), srcBuffer->backing->data());
                    else
                        newBuffer->MarkGpuDirtyImpl();

                    // Since we don't synchost source buffers and the source buffers here are GPU dirty their mirrors will be out of date, meaning the backing contents of this source buffer's region in the new buffer from the initial synchost call will be incorrect. By copying backings directly here we can ensure that no writes are lost and that if the newly created buffer needs to turn GPU dirty during recreation no copies need to be done since the backing is as up to date as the mirror at a minimum.
                    copyBuffer(*newBuffer->guest, *srcBuffer->guest, newBuffer->backing->data(), srcBuffer->backing->data());
                } else if (srcBuffer->AllCpuBackingWritesBlocked()) {
                    if (srcBuffer->dirtyState == Buffer::DirtyState::CpuDirty)
                        LOGE("Buffer (0x{}-0x{}) is marked as CPU dirty while CPU backing writes are blocked, this is not valid", srcBuffer->guest->begin().base(), srcBuffer->guest->end().base());

                    // We need the backing to be stable so that any writes within this context are sequenced correctly, we can't use the source mirror here either since buffer writes within this context will update the mirror on CPU and backing on GPU
                    copyBuffer(*newBuffer->guest, *srcBuffer->guest, newBuffer->backing->data(), srcBuffer->backing->data());
                }
            } else {
                if (srcBuffer->RefreshGpuWritesActiveDirect(false, {})) {
                    newBuffer->MarkGpuDirtyImpl();
                } else if (srcBuffer->directTrackedShadowActive) {
                    newBuffer->EnableTrackedShadowDirect();
                    copyBuffer(*newBuffer->guest, *srcBuffer->guest, newBuffer->directTrackedShadow.data(), srcBuffer->directTrackedShadow.data());
                    newBuffer->directTrackedWrites.Merge(srcBuffer->directTrackedWrites);
                }
            }

            // Transfer all views from the overlapping buffer to the new buffer with the new buffer and updated offset, ensuring pointer stability
            vk::DeviceSize overlapOffset{static_cast<vk::DeviceSize>(srcBuffer->guest->begin() - newBuffer->guest->begin())};
            srcBuffer->delegate->Link(newBuffer->delegate, overlapOffset);
        }

        return newBuffer;
    }

    BufferView BufferManager::FindOrCreateImpl(GuestBuffer guestMapping, ContextTag tag, const std::function<void(std::shared_ptr<Buffer>, ContextLock<Buffer> &&)> &attachBuffer) {
        /*
         * We align the buffer to the page boundary to ensure that:
         * 1) Any buffer view has the same alignment guarantees as on the guest, this is required for UBOs, SSBOs and Texel buffers
         * 2) We can coalesce a lot of tiny buffers into a single large buffer covering an entire page, this is often the case for index buffers and vertex buffers
         */
        auto alignedStart{util::AlignDown(guestMapping.begin().base(), constant::PageSize)}, alignedEnd{util::AlignUp(guestMapping.end().base(), constant::PageSize)};
        span<u8> alignedGuestMapping{alignedStart, alignedEnd};

        auto overlaps{Lookup(alignedGuestMapping, tag)};
        if (overlaps.size() == 1) [[likely]] {
            // If we find a buffer which can entirely fit the guest mapping, we can just return a view into it
            auto &firstOverlap{overlaps.front()};
            if (firstOverlap->guest->begin() <= alignedGuestMapping.begin() && firstOverlap->guest->end() >= alignedGuestMapping.end()) {
                const auto offset{static_cast<vk::DeviceSize>(guestMapping.begin() - firstOverlap->guest->begin())};
                PrepareBufferAccess(**firstOverlap, false, offset, guestMapping.size());
                return firstOverlap->GetView(offset, guestMapping.size());
            }
        }

        if (overlaps.empty()) {
            // If we couldn't find any overlapping buffers, create a new buffer without coalescing
            LockedBuffer buffer{std::make_shared<Buffer>(delegateAllocatorState, gpu, alignedGuestMapping, nextBufferId++, *gpu.state.settings->useDirectMemoryImport), tag};
            buffer->SetupStagedTraps();
            InsertBuffer(*buffer);
            const auto offset{static_cast<vk::DeviceSize>(guestMapping.begin() - buffer->guest->begin())};
            PrepareBufferAccess(**buffer, false, offset, guestMapping.size());
            return buffer->GetView(offset, guestMapping.size());
        } else {
            // If the new buffer overlaps other buffers, we need to create a new buffer and coalesce all overlapping buffers into one
            auto buffer{CoalesceBuffers(alignedGuestMapping, overlaps, tag)};

            // If any overlapping buffer was already attached to the current context, we should also attach the new buffer
            for (auto &srcBuffer : overlaps) {
                if (!srcBuffer.lock.IsFirstUsage()) {
                    attachBuffer(*buffer, std::move(buffer.lock));
                    break;
                }
            }

            // Delete older overlapping buffers and insert the new buffer into the map
            for (auto &overlap : overlaps) {
                DeleteBuffer(*overlap);
                overlap->Invalidate(); // Invalidate the overlapping buffer so it can't be synced in the future
            }
            InsertBuffer(*buffer);

            const auto offset{static_cast<vk::DeviceSize>(guestMapping.begin() - buffer->guest->begin())};
            PrepareBufferAccess(**buffer, false, offset, guestMapping.size());
            return buffer->GetView(offset, guestMapping.size());
        }
    }
}
