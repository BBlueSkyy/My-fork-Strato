// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <memory>
#include <vector>
#include "filesystem.h"
#include "nca.h"

namespace skyline::vfs {

    struct ParsedSparseBucket {
        u32 numberEntries;
        u64 endOffset;
        std::vector<SparseEntry> entries;
    };

    class SparseBacking : public Backing {
      private:
        std::shared_ptr<Backing> rawBacking;
        u64 tableOffset;
        u64 tableSize;
        u64 physicalBaseOffset;

        u32 numBuckets;
        std::vector<u64> bucketVirtualOffsets;
        std::vector<ParsedSparseBucket> buckets;

        void LoadTables();
        u32 FindBucketIndex(size_t target);
        std::pair<SparseEntry, u64> FindSparseEntry(size_t offset);

      public:
        SparseBacking(std::shared_ptr<Backing> raw, u64 tableOffset, u64 tableSize, u64 physicalBaseOffset);
        size_t ReadImpl(span<u8> output, size_t offset) override;
    };

}

