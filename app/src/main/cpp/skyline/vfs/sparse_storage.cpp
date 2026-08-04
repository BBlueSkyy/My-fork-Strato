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

    SparseStorage::SparseStorage(std::shared_ptr<Backing> pBacking, RelocationBlock pBlock, std::vector<RelocationBucket> pBuckets, u64 virtualSize)
        : Backing({true, false, false}, virtualSize), backing(std::move(pBacking)), block(pBlock), buckets(std::move(pBuckets)) {
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

        // TODO: VERIFY before relying on this - fromPatch == 0 is assumed to mean "unallocated, read as
        // zero", which is the documented convention for Nintendo's SparseStorage/IndirectStorage pairing.
        // Confirm against a known-sparse NCA (e.g. log entry.fromPatch/addressSource around the offsets
        // your crash hits, or diff against a real dump) before trusting this in a release build.
        if (!entry.fromPatch) {
            std::fill(output.begin(), output.end(), 0);
            return output.size();
        }

        const u64 physicalOffset{entry.addressSource + (offset - entry.addressPatch)};
        return backing->Read(output, physicalOffset);
    }
}
