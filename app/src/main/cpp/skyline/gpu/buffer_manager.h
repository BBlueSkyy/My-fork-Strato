// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common/trace.h>
#include <common/linear_allocator.h>
#include <common/segment_table.h>
#include <common/spin_lock.h>
#include <unordered_map>
#include "buffer.h"

namespace skyline::gpu {
    /**
     * @brief The Buffer Manager maintains the canonical host representation of guest buffers,
     *        including buffers whose GPU-virtual range resolves to multiple CPU mappings.
     */
    class BufferManager {
      private:
        GPU &gpu;

        struct BufferMapping : span<u8> {
            std::shared_ptr<Buffer> buffer;

            BufferMapping(std::shared_ptr<Buffer> pBuffer, span<u8> mapping)
                : span<u8>{mapping}, buffer{std::move(pBuffer)} {}
        };

        std::vector<BufferMapping> bufferMappings;
        LinearAllocatorState<> delegateAllocatorState;
        size_t nextBufferId{};

        struct ReadOnlyCircularAlias {
            std::weak_ptr<Buffer> source;
            std::shared_ptr<Buffer> alias;
            u32 sourceSequence{};
            size_t sourceSize{};
        };

        std::unordered_map<size_t, ReadOnlyCircularAlias> readOnlyCircularAliases;

        static constexpr size_t L2EntryGranularity{19};
        SegmentTable<Buffer *, constant::AddressSpaceSize, constant::PageSizeBits, L2EntryGranularity> bufferTable;

        struct LockedBuffer {
            std::shared_ptr<Buffer> buffer;
            ContextLock<Buffer> lock;
            std::unique_lock<RecursiveSpinLock> stateLock;

            LockedBuffer(std::shared_ptr<Buffer> pBuffer, ContextTag tag);

            Buffer *operator->() const;
            std::shared_ptr<Buffer> &operator*();
        };

        using LockedBuffers = boost::container::small_vector<LockedBuffer, 4>;

        LockedBuffers Lookup(span<u8> range, ContextTag tag);
        LockedBuffers Lookup(const GuestBuffer &guest, ContextTag tag);

        void InsertBuffer(std::shared_ptr<Buffer> buffer);
        void DeleteBuffer(const std::shared_ptr<Buffer> &buffer);

        LockedBuffer CoalesceBuffers(span<u8> range, const LockedBuffers &srcBuffers, ContextTag tag);
        LockedBuffer CoalesceMappedBuffers(const GuestBuffer &guest, const LockedBuffers &srcBuffers, ContextTag tag);

        static bool BufferLessThan(const BufferMapping &it, u8 *pointer);

      public:
        SpinLock recreationMutex;

        BufferManager(GPU &gpu);

        void lock();
        void unlock();
        bool try_lock();

        BufferView FindOrCreateImpl(span<u8> guestMapping, ContextTag tag,
                                    const std::function<void(std::shared_ptr<Buffer>, ContextLock<Buffer> &&)> &attachBuffer);

        BufferView FindOrCreate(span<u8> guestMapping, ContextTag tag = {},
                                const std::function<void(std::shared_ptr<Buffer>, ContextLock<Buffer> &&)> &attachBuffer = {}) {
            TRACE_EVENT("gpu", "BufferManager::FindOrCreate");
            auto lookupBuffer{bufferTable[guestMapping.begin().base()]};
            if (lookupBuffer != nullptr)
                if (auto view{lookupBuffer->TryGetView(guestMapping)}; view)
                    return view;

            return FindOrCreateImpl(guestMapping, tag, attachBuffer);
        }

        /**
         * @brief Finds or creates one logical buffer backed by an ordered set of CPU mappings.
         * @param guest Page-aligned mappings covering a contiguous GPU-virtual range.
         * @param viewOffset Offset of the requested view within that range.
         * @param viewSize Size of the requested view.
         */
        BufferView FindOrCreate(const GuestBuffer &guest, vk::DeviceSize viewOffset, vk::DeviceSize viewSize,
                                ContextTag tag = {},
                                const std::function<void(std::shared_ptr<Buffer>, ContextLock<Buffer> &&)> &attachBuffer = {});

        /**
         * @brief Creates an independent logical view for a read-only split alias when it cannot
         *        be represented by the canonical overlapping buffer without reordering bytes.
         * @return The view and the owning Buffer. The caller must keep the owner alive while the
         *         view can be referenced.
         * @note This is only allowed while every overlapping canonical buffer has no pending GPU
         *       writes; it must never be used for a writable alias.
         */
        std::pair<BufferView, std::shared_ptr<Buffer>> CreateReadOnlyAlias(
            const GuestBuffer &guest, vk::DeviceSize viewOffset, vk::DeviceSize viewSize,
            ContextTag tag = {});
    };
}
