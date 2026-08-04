// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "sparse_backing.h"
#include <algorithm>
#include <cstring>
#include <common.h>

namespace skyline::vfs {

    template<typename T>
    static T ReadLe(const u8 *ptr) {
        T value{};
        for (size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<T>(ptr[i]) << (8 * i);
        return value;
    }

    static constexpr size_t BktrBucketSize{0x4000};
    static constexpr size_t BktrHeaderSize{0x4000};

    SparseBacking::SparseBacking(std::shared_ptr<Backing> raw, u64 tableOffset, u64 tableSize, u64 physicalBaseOffset)
        : Backing({true, false, false}), rawBacking(std::move(raw)), 
          tableOffset(tableOffset), tableSize(tableSize), physicalBaseOffset(physicalBaseOffset) {
        
        LoadTables();

        if (!buckets.empty()) {
            size = buckets.back().endOffset;
        } else {
            size = rawBacking->size;
        }
    }

    void SparseBacking::LoadTables() {
        std::vector<u8> table(tableSize);
        rawBacking->Read(span(table.data(), table.size()), tableOffset);

        const u8* ptr{table.data()};
        
        if (ReadLe<u32>(ptr + 0x0) != 0x52544B42) {
            throw exception("Invalid BKTR magic in Sparse table");
        }

        numBuckets = ReadLe<u32>(ptr + 0x4); 
        bucketVirtualOffsets.resize(numBuckets);
        buckets.resize(numBuckets);

        for (u32 i = 0; i < numBuckets; ++i) {
            bucketVirtualOffsets[i] = ReadLe<u64>(ptr + 0x10 + static_cast<size_t>(i) * 8);
        }

        for (u32 i = 0; i < numBuckets; ++i) {
            size_t bucketBase{BktrHeaderSize + static_cast<size_t>(i) * BktrBucketSize};
            auto& bucket{buckets[i]};

            bucket.numberEntries = ReadLe<u32>(ptr + bucketBase + 0x4);
            bucket.endOffset = ReadLe<u64>(ptr + bucketBase + 0x8);

            size_t entriesCountOnDisk = static_cast<size_t>(bucket.numberEntries) + 1;
            bucket.entries.resize(entriesCountOnDisk);

            for (size_t e = 0; e < entriesCountOnDisk; ++e) {
                size_t entryBase{bucketBase + 0x10 + e * sizeof(SparseEntry)};
                bucket.entries[e].virtualOffset = ReadLe<u64>(ptr + entryBase + 0x0);
                bucket.entries[e].physicalOffset = ReadLe<u64>(ptr + entryBase + 0x8);
            }
        }
    }

    u32 SparseBacking::FindBucketIndex(size_t target) {
        auto it{std::upper_bound(bucketVirtualOffsets.begin(), bucketVirtualOffsets.end(), target)};
        return static_cast<u32>(std::distance(bucketVirtualOffsets.begin(), it) - 1);
    }

    std::pair<SparseEntry, u64> SparseBacking::FindSparseEntry(size_t offset) {
        if (buckets.empty()) throw exception("Sparse table is empty");

        auto& bucket{buckets.at(FindBucketIndex(offset))};

        int64_t low{0}, high{static_cast<int64_t>(bucket.numberEntries) - 1};
        while (low <= high) {
            int64_t mid{low + (high - low) / 2};
            auto& entry{bucket.entries[static_cast<size_t>(mid)]};
            
            if (entry.virtualOffset > offset) {
                high = mid - 1;
            } else {
                auto& next{bucket.entries[static_cast<size_t>(mid) + 1]};
                if (next.virtualOffset > offset)
                    return {entry, next.virtualOffset};
                low = mid + 1;
            }
        }
        throw exception("Failed to resolve Sparse entry");
    }

    size_t SparseBacking::ReadImpl(span<u8> output, size_t offset) {
        size_t totalRead{0};

        while (totalRead < output.size()) {
            size_t virtOffset{offset + totalRead};
            auto [entry, regionEnd] = FindSparseEntry(virtOffset);
            size_t chunk{std::min(output.size() - totalRead, regionEnd - virtOffset)};

            if (entry.physicalOffset == 0) {
                std::memset(output.data() + totalRead, 0, chunk);
            } else {
                size_t offsetInEntry = virtOffset - entry.virtualOffset;
                size_t physReadOffset = physicalBaseOffset + entry.physicalOffset + offsetInEntry;
                rawBacking->Read(output.subspan(totalRead, chunk), physReadOffset);
            }

            totalRead += chunk;
        }

        return totalRead;
    }
}
