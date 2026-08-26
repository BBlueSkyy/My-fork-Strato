// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include <lz4.h>
#include <limits>

#include "compressed_storage.h"

namespace skyline::vfs {
    static std::pair<u64, u64> SearchBucketEntry(u64 offset, const RelocationBlock &block, const std::vector<CompressedBucket> &buckets) {
        if (block.numberBuckets == 0 || block.numberBuckets > block.baseOffsets.size() || block.numberBuckets > buckets.size())
            throw exception("CompressedStorage: invalid bucket count {}", block.numberBuckets);

        auto bucketIt{std::upper_bound(block.baseOffsets.begin(), block.baseOffsets.begin() + block.numberBuckets, offset)};
        if (bucketIt == block.baseOffsets.begin())
            throw exception("CompressedStorage: offset 0x{:X} is before the first bucket", offset);

        const u64 bucketId{static_cast<u64>(std::distance(block.baseOffsets.begin(), bucketIt) - 1)};

        const auto &bucket{buckets[bucketId]};
        if (bucket.numberEntries == 0 || bucket.numberEntries > bucket.entries.size())
            throw exception("CompressedStorage: invalid entry count {} in bucket {}", bucket.numberEntries, bucketId);

        if (bucket.numberEntries == 1)
            return {bucketId, 0};

        auto entryIt{std::upper_bound(bucket.entries.begin(), bucket.entries.begin() + bucket.numberEntries, offset, [](u64 offset, const auto &entry) {
            return offset < entry.virtualOffset;
        })};

        if (entryIt != bucket.entries.begin()) {
            u64 entryIndex{static_cast<u64>(std::distance(bucket.entries.begin(), entryIt) - 1)};
            return {bucketId, entryIndex};
        }

        throw exception("CompressedStorage: offset 0x{:X} could not be resolved in the bucket tree", offset);
    }

    CompressedStorage::CompressedStorage(std::shared_ptr<Backing> pBacking, RelocationBlock pBlock, std::vector<CompressedBucket> pBuckets, u64 virtualSize)
        : Backing({true, false, false}, virtualSize), backing(std::move(pBacking)), block(pBlock), buckets(std::move(pBuckets)) {
        if (!backing)
            throw exception("CompressedStorage: null data backing");
        if (block.index != 0 || block.numberBuckets == 0 || block.numberBuckets > block.baseOffsets.size() || block.numberBuckets != buckets.size())
            throw exception("CompressedStorage: invalid root node (index={}, buckets={})", block.index, block.numberBuckets);
        if (block.size == 0 || virtualSize != block.size)
            throw exception("CompressedStorage: invalid virtual size 0x{:X} (tree=0x{:X})", virtualSize, block.size);

        for (std::size_t i{}; i < block.numberBuckets - 1; ++i)
            buckets[i].entries.push_back({block.baseOffsets[i + 1], 0, NCACompressionType::None, 0, {}, 0});

        buckets[block.numberBuckets - 1].entries.push_back({block.size, 0, NCACompressionType::None, 0, {}, 0});
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
        if (index.first + 1 >= buckets.size() || buckets[index.first + 1].entries.empty())
            throw exception("CompressedStorage: missing next entry for bucket {}", index.first);
        return buckets[index.first + 1].entries.front();
    }

    size_t CompressedStorage::ReadImpl(span<u8> output, size_t offset) {
        if (output.empty())
            return 0;

        if (offset >= block.size)
            return 0;

        const auto entry{GetEntry(offset)};
        const auto next{GetNextEntry(offset)};

        if (next.virtualOffset <= offset)
            throw exception("CompressedStorage: non-advancing entry boundary at offset 0x{:X}", offset);

        if (output.size() > next.virtualOffset - offset) {
            const u64 partition{next.virtualOffset - offset};
            span<u8> tail(output.data() + partition, output.size() - partition);
            return ReadImpl(output.subspan(0, partition), offset) + ReadImpl(tail, offset + partition);
        }

        if (entry.virtualOffset > offset)
            throw exception("CompressedStorage: entry starts after requested offset 0x{:X}", offset);

        const u64 offsetInBlock{offset - entry.virtualOffset};
        const u64 decompressedBlockSize{next.virtualOffset - entry.virtualOffset};
        if (offsetInBlock > decompressedBlockSize || output.size() > decompressedBlockSize - offsetInBlock)
            throw exception("CompressedStorage: virtual read exceeds entry boundary");

        switch (entry.compressionType) {
            case NCACompressionType::Zeroed:
                std::fill(output.begin(), output.end(), 0);
                return output.size();

            case NCACompressionType::None:
                if (entry.physicalOffset > backing->size || offsetInBlock > backing->size - entry.physicalOffset ||
                    output.size() > backing->size - entry.physicalOffset - offsetInBlock)
                    throw exception("CompressedStorage: raw physical read out of range");
                return backing->Read(output, entry.physicalOffset + offsetInBlock);

            case NCACompressionType::Lz4: {
                if (entry.physicalSize == 0 || entry.physicalOffset > backing->size || entry.physicalSize > backing->size - entry.physicalOffset)
                    throw exception("CompressedStorage: LZ4 physical read out of range");
                if (entry.physicalSize > static_cast<u64>(std::numeric_limits<int>::max()) ||
                    decompressedBlockSize > static_cast<u64>(std::numeric_limits<int>::max()))
                    throw exception("CompressedStorage: LZ4 block exceeds decoder limits");

                std::scoped_lock lock{cacheMutex};
                if (!cachedBlockVirtualOffset || *cachedBlockVirtualOffset != entry.virtualOffset) {
                    std::vector<u8> compressed(entry.physicalSize);
                    if (backing->Read(compressed, entry.physicalOffset) != compressed.size())
                        throw exception("CompressedStorage: short read for LZ4 block at 0x{:X}", entry.physicalOffset);

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
