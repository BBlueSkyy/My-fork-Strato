// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/settings.h>
#include <gpu.h>
#include "buffer_manager.h"

namespace skyline::gpu {
    BufferManager::BufferManager(GPU &gpu) : gpu{gpu} {}

    bool BufferManager::BufferLessThan(const BufferMapping &it, u8 *pointer) {
        return it.begin().base() < pointer;
    }

    BufferManager::LockedBuffer::LockedBuffer(std::shared_ptr<Buffer> pBuffer, ContextTag tag)
        : buffer{std::move(pBuffer)}, lock{tag, *buffer}, stateLock(buffer->stateMutex) {}

    Buffer *BufferManager::LockedBuffer::operator->() const {
        return buffer.get();
    }

    std::shared_ptr<Buffer> &BufferManager::LockedBuffer::operator*() {
        return buffer;
    }

    BufferManager::LockedBuffers BufferManager::Lookup(span<u8> range, ContextTag tag) {
        boost::container::small_vector<std::shared_ptr<Buffer>, 4> matches;

        auto addUnique{[&](const std::shared_ptr<Buffer> &buffer) {
            if (std::find(matches.begin(), matches.end(), buffer) == matches.end())
                matches.emplace_back(buffer);
        }};

        auto lookupBuffer{bufferTable[range.begin().base()]};
        if (lookupBuffer != nullptr)
            addUnique(lookupBuffer->shared_from_this());

        auto entryIt{std::lower_bound(bufferMappings.begin(), bufferMappings.end(), range.end().base(), BufferLessThan)};
        while (entryIt != bufferMappings.begin()) {
            auto &entry{*--entryIt};
            if (entry.end().base() > range.begin().base() && entry.begin().base() < range.end().base())
                addUnique(entry.buffer);
        }

        LockedBuffers overlaps;
        overlaps.reserve(matches.size());
        for (auto &buffer : matches)
            overlaps.emplace_back(buffer, tag);
        return overlaps;
    }

    BufferManager::LockedBuffers BufferManager::Lookup(const GuestBuffer &guest, ContextTag tag) {
        boost::container::small_vector<std::shared_ptr<Buffer>, 8> matches;

        auto addUnique{[&](const std::shared_ptr<Buffer> &buffer) {
            if (std::find(matches.begin(), matches.end(), buffer) == matches.end())
                matches.emplace_back(buffer);
        }};

        for (const auto &range : guest.mappings) {
            auto lookupBuffer{bufferTable[range.begin().base()]};
            if (lookupBuffer != nullptr)
                addUnique(lookupBuffer->shared_from_this());

            auto entryIt{std::lower_bound(bufferMappings.begin(), bufferMappings.end(), range.end().base(), BufferLessThan)};
            while (entryIt != bufferMappings.begin()) {
                auto &entry{*--entryIt};
                if (entry.end().base() > range.begin().base() && entry.begin().base() < range.end().base())
                    addUnique(entry.buffer);
            }
        }

        LockedBuffers overlaps;
        overlaps.reserve(matches.size());
        for (auto &buffer : matches)
            overlaps.emplace_back(buffer, tag);
        return overlaps;
    }

    void BufferManager::InsertBuffer(std::shared_ptr<Buffer> buffer) {
        for (const auto &mapping : buffer->guest->mappings) {
            bufferTable.Set(mapping.begin().base(), mapping.end().base(), buffer.get());
            auto position{std::lower_bound(bufferMappings.begin(), bufferMappings.end(), mapping.begin().base(), BufferLessThan)};
            bufferMappings.emplace(position, buffer, mapping);
        }
    }

    void BufferManager::DeleteBuffer(const std::shared_ptr<Buffer> &buffer) {
        for (const auto &mapping : buffer->guest->mappings)
            bufferTable.Set(mapping.begin().base(), mapping.end().base(), nullptr);

        std::erase_if(bufferMappings, [&](const BufferMapping &mapping) {
            return mapping.buffer == buffer;
        });
    }

    BufferManager::LockedBuffer BufferManager::CoalesceBuffers(span<u8> range, const LockedBuffers &srcBuffers, ContextTag tag) {
        TRACE_EVENT("gpu", "BufferManager::CoalesceBuffers");

        for (const auto &srcBuffer : srcBuffers)
            if (srcBuffer->guest->mappings.size() != 1)
                throw exception("Cannot fold a non-contiguous guest buffer into a physically contiguous buffer");

        std::shared_ptr<FenceCycle> newBufferCycle{};
        for (auto &srcBuffer : srcBuffers) {
            if (!*gpu.state.settings->useDirectMemoryImport &&
                (srcBuffer->dirtyState == Buffer::DirtyState::GpuDirty || srcBuffer->AllCpuBackingWritesBlocked()))
                srcBuffer->WaitOnFence();

            if (newBufferCycle && srcBuffer->cycle != newBufferCycle)
                srcBuffer->WaitOnFence();
            else
                newBufferCycle = srcBuffer->cycle;
        }

        std::scoped_lock lock{recreationMutex};
        if (!range.valid())
            range = span<u8>{srcBuffers.front().buffer->guest->mappings.front().begin(),
                             srcBuffers.back().buffer->guest->mappings.front().end()};

        auto lowestAddress{range.begin().base()}, highestAddress{range.end().base()};
        for (const auto &srcBuffer : srcBuffers) {
            auto mapping{srcBuffer->guest->mappings.front()};
            if (mapping.begin().base() < lowestAddress)
                lowestAddress = mapping.begin().base();
            if (mapping.end().base() > highestAddress)
                highestAddress = mapping.end().base();
        }

        GuestBuffer guest{span<u8>{lowestAddress, highestAddress}};
        LockedBuffer newBuffer{std::make_shared<Buffer>(delegateAllocatorState, gpu, guest, nextBufferId++,
                                                        *gpu.state.settings->useDirectMemoryImport), tag};

        newBuffer->SetupStagedTraps();
        newBuffer->SynchronizeHost(false);
        newBuffer->cycle = newBufferCycle;

        auto copyBuffer{[](span<u8> dstGuest, span<u8> srcGuest, u8 *dstPtr, const u8 *srcPtr) {
            if (dstGuest.begin().base() <= srcGuest.begin().base()) {
                size_t dstOffset{static_cast<size_t>(srcGuest.begin().base() - dstGuest.begin().base())};
                size_t copySize{std::min(dstGuest.size() - dstOffset, srcGuest.size())};
                std::memcpy(dstPtr + dstOffset, srcPtr, copySize);
            } else {
                size_t srcOffset{static_cast<size_t>(dstGuest.begin().base() - srcGuest.begin().base())};
                size_t copySize{std::min(dstGuest.size(), srcGuest.size() - srcOffset)};
                std::memcpy(dstPtr, srcPtr + srcOffset, copySize);
            }
        }};

        auto mergeTrackedWrites{[](Buffer &dst, Buffer &src, size_t dstOffset, size_t srcSize) {
            for (size_t offset{}; offset < srcSize;) {
                auto interval{src.directTrackedWrites.Query(offset)};
                if (interval.enclosed)
                    dst.directTrackedWrites.Insert({dstOffset + offset, dstOffset + offset + interval.size});
                if (!interval.size)
                    break;
                offset += interval.size;
            }
        }};

        auto dstGuest{newBuffer->guest->mappings.front()};
        for (auto &srcBuffer : srcBuffers) {
            auto srcGuest{srcBuffer->guest->mappings.front()};

            if (newBuffer->backingImmutability == Buffer::BackingImmutability::None &&
                srcBuffer->backingImmutability != Buffer::BackingImmutability::None)
                newBuffer->backingImmutability = srcBuffer->backingImmutability;
            else if (srcBuffer->backingImmutability == Buffer::BackingImmutability::AllWrites)
                newBuffer->backingImmutability = Buffer::BackingImmutability::AllWrites;

            newBuffer->everHadInlineUpdate |= srcBuffer->everHadInlineUpdate;

            if (!*gpu.state.settings->useDirectMemoryImport) {
                if (srcBuffer->dirtyState == Buffer::DirtyState::GpuDirty) {
                    if (srcBuffer.lock.IsFirstUsage() && newBuffer->dirtyState != Buffer::DirtyState::GpuDirty)
                        copyBuffer(dstGuest, srcGuest, newBuffer->mirror.data(), srcBuffer->backing->data());
                    else
                        newBuffer->MarkGpuDirtyImpl();

                    copyBuffer(dstGuest, srcGuest, newBuffer->backing->data(), srcBuffer->backing->data());
                } else if (srcBuffer->AllCpuBackingWritesBlocked()) {
                    if (srcBuffer->dirtyState == Buffer::DirtyState::CpuDirty)
                        LOGE("Buffer is CPU dirty while CPU backing writes are blocked");
                    copyBuffer(dstGuest, srcGuest, newBuffer->backing->data(), srcBuffer->backing->data());
                }
            } else {
                if (srcBuffer->RefreshGpuWritesActiveDirect(false, {})) {
                    newBuffer->MarkGpuDirtyImpl();
                } else if (srcBuffer->directTrackedShadowActive) {
                    newBuffer->EnableTrackedShadowDirect();
                    copyBuffer(dstGuest, srcGuest, newBuffer->directTrackedShadow.data(), srcBuffer->directTrackedShadow.data());
                    auto dstOffset{static_cast<size_t>(srcGuest.begin().base() - dstGuest.begin().base())};
                    mergeTrackedWrites(*newBuffer, *srcBuffer, dstOffset, srcGuest.size());
                }
            }

            vk::DeviceSize overlapOffset{static_cast<vk::DeviceSize>(srcGuest.begin() - dstGuest.begin())};
            srcBuffer->delegate->Link(newBuffer->delegate, overlapOffset);
        }

        return newBuffer;
    }

    BufferManager::LockedBuffer BufferManager::CoalesceMappedBuffers(const GuestBuffer &guest, const LockedBuffers &srcBuffers, ContextTag tag) {
        TRACE_EVENT("gpu", "BufferManager::CoalesceMappedBuffers");

        boost::container::small_vector<size_t, 8> dstOffsets;
        dstOffsets.reserve(srcBuffers.size());
        for (const auto &srcBuffer : srcBuffers) {
            auto offset{guest.Find(*srcBuffer->guest)};
            if (!offset)
                throw exception("Incompatible guest buffer alias layout while resolving split mappings");
            dstOffsets.emplace_back(*offset);
        }

        std::shared_ptr<FenceCycle> newBufferCycle{};
        for (auto &srcBuffer : srcBuffers) {
            if (!*gpu.state.settings->useDirectMemoryImport &&
                (srcBuffer->dirtyState == Buffer::DirtyState::GpuDirty || srcBuffer->AllCpuBackingWritesBlocked()))
                srcBuffer->WaitOnFence();

            if (newBufferCycle && srcBuffer->cycle != newBufferCycle)
                srcBuffer->WaitOnFence();
            else
                newBufferCycle = srcBuffer->cycle;
        }

        std::scoped_lock lock{recreationMutex};
        LockedBuffer newBuffer{std::make_shared<Buffer>(delegateAllocatorState, gpu, guest, nextBufferId++,
                                                        *gpu.state.settings->useDirectMemoryImport), tag};

        newBuffer->SetupStagedTraps();
        newBuffer->SynchronizeHost(false);
        newBuffer->cycle = newBufferCycle;

        auto mergeTrackedWrites{[](Buffer &dst, Buffer &src, size_t dstOffset, size_t srcSize) {
            for (size_t offset{}; offset < srcSize;) {
                auto interval{src.directTrackedWrites.Query(offset)};
                if (interval.enclosed)
                    dst.directTrackedWrites.Insert({dstOffset + offset, dstOffset + offset + interval.size});
                if (!interval.size)
                    break;
                offset += interval.size;
            }
        }};

        for (size_t i{}; i < srcBuffers.size(); ++i) {
            auto &srcBuffer{srcBuffers[i]};
            size_t dstOffset{dstOffsets[i]};
            size_t srcSize{srcBuffer->guest->size()};

            if (newBuffer->backingImmutability == Buffer::BackingImmutability::None &&
                srcBuffer->backingImmutability != Buffer::BackingImmutability::None)
                newBuffer->backingImmutability = srcBuffer->backingImmutability;
            else if (srcBuffer->backingImmutability == Buffer::BackingImmutability::AllWrites)
                newBuffer->backingImmutability = Buffer::BackingImmutability::AllWrites;

            newBuffer->everHadInlineUpdate |= srcBuffer->everHadInlineUpdate;

            if (!*gpu.state.settings->useDirectMemoryImport) {
                if (srcBuffer->dirtyState == Buffer::DirtyState::GpuDirty) {
                    if (srcBuffer.lock.IsFirstUsage() && newBuffer->dirtyState != Buffer::DirtyState::GpuDirty)
                        std::memcpy(newBuffer->mirror.data() + dstOffset, srcBuffer->backing->data(), srcSize);
                    else
                        newBuffer->MarkGpuDirtyImpl();

                    std::memcpy(newBuffer->backing->data() + dstOffset, srcBuffer->backing->data(), srcSize);
                } else if (srcBuffer->AllCpuBackingWritesBlocked()) {
                    if (srcBuffer->dirtyState == Buffer::DirtyState::CpuDirty)
                        LOGE("Buffer is CPU dirty while CPU backing writes are blocked");
                    std::memcpy(newBuffer->backing->data() + dstOffset, srcBuffer->backing->data(), srcSize);
                }
            } else {
                if (srcBuffer->RefreshGpuWritesActiveDirect(false, {})) {
                    newBuffer->MarkGpuDirtyImpl();
                } else if (srcBuffer->directTrackedShadowActive) {
                    newBuffer->EnableTrackedShadowDirect();
                    std::memcpy(newBuffer->directTrackedShadow.data() + dstOffset,
                                srcBuffer->directTrackedShadow.data(), srcSize);
                    mergeTrackedWrites(*newBuffer, *srcBuffer, dstOffset, srcSize);
                }
            }

            srcBuffer->delegate->Link(newBuffer->delegate, dstOffset);
        }

        return newBuffer;
    }

    BufferView BufferManager::FindOrCreateImpl(span<u8> guestMapping, ContextTag tag,
                                               const std::function<void(std::shared_ptr<Buffer>, ContextLock<Buffer> &&)> &attachBuffer) {
        auto alignedStart{util::AlignDown(guestMapping.begin().base(), constant::PageSize)};
        auto alignedEnd{util::AlignUp(guestMapping.end().base(), constant::PageSize)};
        span<u8> alignedGuestMapping{alignedStart, alignedEnd};

        auto overlaps{Lookup(alignedGuestMapping, tag)};
        if (overlaps.size() == 1) [[likely]] {
            auto &firstOverlap{overlaps.front()};
            if (auto view{firstOverlap->TryGetView(alignedGuestMapping)}; view)
                return firstOverlap->TryGetView(guestMapping);
        }

        if (overlaps.empty()) {
            LockedBuffer buffer{std::make_shared<Buffer>(delegateAllocatorState, gpu, GuestBuffer{alignedGuestMapping},
                                                         nextBufferId++, *gpu.state.settings->useDirectMemoryImport), tag};
            buffer->SetupStagedTraps();
            InsertBuffer(*buffer);
            return buffer->TryGetView(guestMapping);
        }

        auto buffer{CoalesceBuffers(alignedGuestMapping, overlaps, tag)};

        for (auto &srcBuffer : overlaps) {
            if (!srcBuffer.lock.IsFirstUsage()) {
                attachBuffer(*buffer, std::move(buffer.lock));
                break;
            }
        }

        for (auto &overlap : overlaps) {
            DeleteBuffer(*overlap);
            overlap->Invalidate();
        }
        InsertBuffer(*buffer);

        return buffer->TryGetView(guestMapping);
    }

    BufferView BufferManager::FindOrCreate(const GuestBuffer &guest, vk::DeviceSize viewOffset, vk::DeviceSize viewSize,
                                           ContextTag tag,
                                           const std::function<void(std::shared_ptr<Buffer>, ContextLock<Buffer> &&)> &attachBuffer) {
        TRACE_EVENT("gpu", "BufferManager::FindOrCreateMapped");

        if (!guest.valid() || viewOffset > guest.size() || viewSize > guest.size() - viewOffset)
            return {};

        auto lookupBuffer{bufferTable[guest.mappings.front().begin().base()]};
        if (lookupBuffer != nullptr)
            if (auto view{lookupBuffer->TryGetView(guest, viewOffset, viewSize)}; view)
                return view;

        auto overlaps{Lookup(guest, tag)};
        for (auto &overlap : overlaps)
            if (auto view{overlap->TryGetView(guest, viewOffset, viewSize)}; view)
                return view;

        if (overlaps.empty()) {
            LockedBuffer buffer{std::make_shared<Buffer>(delegateAllocatorState, gpu, guest, nextBufferId++,
                                                         *gpu.state.settings->useDirectMemoryImport), tag};
            buffer->SetupStagedTraps();
            InsertBuffer(*buffer);
            return buffer->TryGetView(guest, viewOffset, viewSize);
        }

        auto buffer{CoalesceMappedBuffers(guest, overlaps, tag)};

        for (auto &srcBuffer : overlaps) {
            if (!srcBuffer.lock.IsFirstUsage()) {
                attachBuffer(*buffer, std::move(buffer.lock));
                break;
            }
        }

        for (auto &overlap : overlaps) {
            DeleteBuffer(*overlap);
            overlap->Invalidate();
        }
        InsertBuffer(*buffer);

        return buffer->TryGetView(guest, viewOffset, viewSize);
    }
}
