// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include <lz4.h>

#include "compressed_storage.h"

namespace skyline::vfs {
    // Same bucket search strategy as SparseStorage - the L1 node format (RelocationBlock) doesn't depend
    // on the leaf entry size, so this is identical apart from the entry type and comparison field name
    static std::pair<u64, u64> SearchBucketEntry(u64 offset, const RelocationBlock &block, const std::vector<CompressedBucket> &buckets) {
        u64 bucketId{static_cast<u64>(std::distance(block.baseOffsets.begin(),
                                                    std::upper_bound(block.baseOffsets.begin() + 1,
                                                                     block.baseOffsets.begin() + block.numberBuckets, offset)) - 1)};

        const auto &bucket{buckets[bucketId]};

        if (bucket.numberEntries == 1)
            return {bucketId, 0};

        auto entryIt{std::upper_bound(bucket.entries.begin(), bucket.entries.begin() + bucket.numberEntries, offset, [](u64 offset, const auto &entry) {
            return offset < entry.virtualOffset;
        })};

        if (entryIt != bucket.entries.begin()) {
            u64 entryIndex{static_cast<u64>(std::distance(bucket.entries.begin(), entryIt) - 1)};
            return {bucketId, entryIndex};
        }

        LOGE("CompressedStorage: Offset 0x{:X} could not be resolved in the bucket tree", offset);
        return {0, 0};
    }

    CompressedStorage::CompressedStorage(std::shared_ptr<Backing> pBacking, RelocationBlock pBlock, std::vector<CompressedBucket> pBuckets, u64 virtualSize)
        : Backing({true, false, false}, virtualSize), backing(std::move(pBacking)), block(pBlock), buckets(std::move(pBuckets)) {
        // Cap every bucket with a sentinel entry pointing at the start of the next bucket, so
        // GetNextEntry() always has a following entry to bound a read/block against. The sentinel's own
        // fields are never dereferenced for data, only its virtualOffset is read as a boundary.
        for (std::size_t i{}; i < block.numberBuckets - 1; ++i)
            buckets[i].entries.push_back({block.baseOffsets[i + 1], 0, NCACompressionType::None, 0, {}, 0});

        buckets.back().entries.push_back({block.size, 0, NCACompressionType::None, 0, {}, 0});
    }

    std::pair<u64, u64> CompressedStorage::GetEntryIndex(u64 offset) {
        return SearchBucketEntry(offset, block, buckets);
    }

    CompressedEntry CompressedStorage::GetEntry(u64 offset) {
        const auto index{GetEntryIndex(offset)};
        return buckets[index.first].entries[index.second];
    }

    CompressedEntry CompressedStorage::GetNextEntry(u64 offset) {
        const auto index{GetEntryIndex(offset)};
        const auto &bucket{buckets[index.first]};
        if (index.second + 1 < bucket.entries.size())
            return bucket.entries[index.second + 1];
        return buckets[index.first + 1].entries[0];
    }

    size_t CompressedStorage::ReadImpl(span<u8> output, size_t offset) {
        if (offset >= block.size)
            return 0;

        const auto entry{GetEntry(offset)};
        const auto next{GetNextEntry(offset)};

        // Split at entry boundaries first, same as SparseStorage/BKTR, so a single call never straddles
        // two blocks that could have different compression types
        if (offset + output.size() > next.virtualOffset) {
            const u64 partition{next.virtualOffset - offset};
            if (partition == 0)
                throw exception("CompressedStorage: Bucket tree returned a non-advancing entry at offset 0x{:X} (entry.virtualOffset=0x{:X}, next.virtualOffset=0x{:X}) - would recurse forever", offset, entry.virtualOffset, next.virtualOffset);
            span<u8> tail(output.data() + partition, output.size() - partition);
            return ReadImpl(output.subspan(0, partition), offset) + ReadImpl(tail, offset + partition);
        }

        const u64 offsetInBlock{offset - entry.virtualOffset};
        const u64 decompressedBlockSize{next.virtualOffset - entry.virtualOffset};

        switch (entry.compressionType) {
            case NCACompressionType::Zeroed:
                std::fill(output.begin(), output.end(), 0);
                return output.size();

            case NCACompressionType::None:
                return backing->Read(output, entry.physicalOffset + offsetInBlock);

            case NCACompressionType::Lz4: {
                // The reference implementation always decompresses the entire block even for a partial
                // read, so a run of small sequential reads landing in the same block (routine while
                // parsing RomFS/IVFC metadata) would otherwise redo a full 64KB decompression per read -
                // cache the last block by its entry.virtualOffset and only redecompress on a miss
                if (!cachedBlockVirtualOffset || *cachedBlockVirtualOffset != entry.virtualOffset) {
                    std::vector<u8> compressed(entry.physicalSize);
                    backing->Read(compressed, entry.physicalOffset);

                    cachedBlock.resize(decompressedBlockSize);
                    int result{LZ4_decompress_safe(reinterpret_cast<const char *>(compressed.data()), reinterpret_cast<char *>(cachedBlock.data()),
                                                    static_cast<int>(compressed.size()), static_cast<int>(cachedBlock.size()))};

                    if (result < 0 || static_cast<size_t>(result) != cachedBlock.size()) {
                        cachedBlockVirtualOffset.reset();
                        throw exception("CompressedStorage: LZ4 decompression failed for block at virtual offset 0x{:X}", entry.virtualOffset);
                    }

                    cachedBlockVirtualOffset = entry.virtualOffset;
                }

                std::copy(cachedBlock.begin() + static_cast<ssize_t>(offsetInBlock), cachedBlock.begin() + static_cast<ssize_t>(offsetInBlock + output.size()), output.begin());
                return output.size();
            }

            default:
                throw exception("CompressedStorage: Unhandled compression type {} at virtual offset 0x{:X}", static_cast<u8>(entry.compressionType), entry.virtualOffset);
        }
    }
}
