// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include "sparse_storage.h"

namespace skyline::vfs {
    // Mirrors BKTR's SearchBucketEntry (bktr.cpp) minus the subsection-specific last-bucket shortcut,
    // which doesn't apply here since Sparse only ever deals with one kind of bucket tree.
    static std::pair<u64, u64> SearchBucketEntry(u64 offset, const RelocationBlock &block, const std::vector<RelocationBucket> &buckets) {
        u64 bucketId{static_cast<u64>(std::distance(block.baseOffsets.begin(),
                                                    std::upper_bound(block.baseOffsets.begin() + 1,
                                                                     block.baseOffsets.begin() + block.numberBuckets, offset)) - 1)};

        const auto &bucket{buckets[bucketId]};

        if (bucket.numberEntries == 1)
            return {bucketId, 0};

        auto entryIt{std::upper_bound(bucket.entries.begin(), bucket.entries.begin() + bucket.numberEntries, offset, [](u64 offset, const auto &entry) {
            return offset < entry.addressPatch;
        })};

        if (entryIt != bucket.entries.begin()) {
            u64 entryIndex{static_cast<u64>(std::distance(bucket.entries.begin(), entryIt) - 1)};
            return {bucketId, entryIndex};
        }

        LOGE("SparseStorage: Offset 0x{:X} could not be resolved in the bucket tree", offset);
        return {0, 0};
    }

    SparseStorage::SparseStorage(std::shared_ptr<Backing> pBacking, RelocationBlock pBlock, std::vector<RelocationBucket> pBuckets, u64 virtualSize, u64 pPhysicalBaseOffset)
        : Backing({true, false, false}, virtualSize), backing(std::move(pBacking)), block(pBlock), buckets(std::move(pBuckets)), physicalBaseOffset(pPhysicalBaseOffset) {
        // Cap every bucket with a sentinel entry pointing at the start of the next bucket, exactly like
        // BKTR's constructor does, so GetNextEntry() always has a following entry to bound a read against
        for (std::size_t i{}; i < block.numberBuckets - 1; ++i)
            buckets[i].entries.push_back({block.baseOffsets[i + 1], 0, 0});

        buckets.back().entries.push_back({block.size, 0, 0});
    }

    std::pair<u64, u64> SparseStorage::GetEntryIndex(u64 offset) {
        return SearchBucketEntry(offset, block, buckets);
    }

    RelocationEntry SparseStorage::GetEntry(u64 offset) {
        const auto index{GetEntryIndex(offset)};
        return buckets[index.first].entries[index.second];
    }

    RelocationEntry SparseStorage::GetNextEntry(u64 offset) {
        const auto index{GetEntryIndex(offset)};
        const auto &bucket{buckets[index.first]};
        if (index.second + 1 < bucket.entries.size())
            return bucket.entries[index.second + 1];
        return buckets[index.first + 1].entries[0];
    }

    size_t SparseStorage::ReadImpl(span<u8> output, size_t offset) {
        if (offset >= block.size)
            return 0;

        const auto entry{GetEntry(offset)};
        const auto next{GetNextEntry(offset)};

        // Split the read at the entry boundary, the same strategy BKTR uses, so a single request never
        // straddles two entries that could resolve to different physical regions (or zero vs. real data)
        if (offset + output.size() > next.addressPatch) {
            const u64 partition{next.addressPatch - offset};
            span<u8> tail(output.data() + partition, output.size() - partition);
            return ReadImpl(output.subspan(0, partition), offset) + ReadImpl(tail, offset + partition);
        }

        // Confirmed against LibHac's SparseStorage: storage index/fromPatch 0 is the real data storage,
        // 1 is the always-zero storage (SetZeroStorage always assigns index 1) - so a *nonzero* fromPatch
        // means "never physically stored, read as zero", not the other way around.
        if (entry.fromPatch) {
            std::fill(output.begin(), output.end(), 0);
            return output.size();
        }

        // Entry physical offsets are relative to NCASparseInfo::physicalOffset, not to the start of
        // the backing - the compacted real data doesn't necessarily begin at offset 0.
        const u64 physicalOffset{physicalBaseOffset + entry.addressSource + (offset - entry.addressPatch)};
        return backing->Read(output, physicalOffset);
    }
}
