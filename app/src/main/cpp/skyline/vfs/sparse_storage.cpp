// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include "sparse_storage.h"
#include "region_backing.h"
#include "ctr_encrypted_backing.h"

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

    SparseStorage::SparseStorage(std::shared_ptr<Backing> pRawBacking, crypto::KeyStore::Key128 pKey, std::array<u8, 0x10> pCtr, size_t pSectionPhysicalStart,
                              RelocationBlock pBlock, std::vector<RelocationBucket> pBuckets, u64 virtualSize, u64 pPhysicalBaseOffset)
    : Backing({true, false, false}, virtualSize), rawBacking(std::move(pRawBacking)), key(pKey), ctr(pCtr), sectionPhysicalStart(pSectionPhysicalStart),
    block(pBlock), buckets(std::move(pBuckets)), physicalBaseOffset(pPhysicalBaseOffset) {
    for (std::size_t i{}; i < block.numberBuckets - 1; ++i)
        buckets[i].entries.push_back({block.baseOffsets[i + 1], 0, 0});
        buckets.back().entries.push_back({block.size, 0, 0});
    }

      // GetEntryIndex / GetEntry / GetNextEntry ficam exatamente como estão hoje

      size_t SparseStorage::ReadImpl(span<u8> output, size_t offset) {
      if (offset >= block.size)
        return 0;

    const auto entry{GetEntry(offset)};
    const auto next{GetNextEntry(offset)};

    if (offset + output.size() > next.addressPatch) {
        const u64 partition{next.addressPatch - offset};
        span<u8> tail(output.data() + partition, output.size() - partition);
        return ReadImpl(output.subspan(0, partition), offset) + ReadImpl(tail, offset + partition);
    }

    if (entry.fromPatch) {
        std::fill(output.begin(), output.end(), 0);
        return output.size();
    }

      // This block was encrypted as if it sat at its *virtual* offset, not wherever it was physically
      // compacted to. RegionBacking absorbs the constant physical-minus-virtual delta for this entry as its
      // base, so CtrEncryptedBacking's own offset - which is what its counter is derived from - can stay the
      // true virtual `offset`. `regionBase` intentionally wraps as an unsigned value when physical < virtual;
      // adding back `offset` (always >= entry.addressPatch here) correctly unwraps it - standard, safe
      // modular arithmetic, not a bug.
      const u64 physicalOffset{physicalBaseOffset + entry.addressSource + (offset - entry.addressPatch)};
      const u64 regionBase{sectionPhysicalStart + physicalOffset - offset};

       auto region{std::make_shared<RegionBacking>(rawBacking, regionBase, rawBacking->size)};
        CtrEncryptedBacking decryptor{ctr, key, region, sectionPhysicalStart};
        return decryptor.ReadUnchecked(output, offset);
    }
}
