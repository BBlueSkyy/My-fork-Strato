// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <array>
#include <memory>
#include <vector>

#include <crypto/key_store.h>
#include "backing.h"

namespace skyline::vfs {
    class BktrBacking : public Backing {
      private:
        struct RelocEntry {
            u64 virtOffset;
            u64 physOffset;
            u32 isPatch;
            u32 _pad_;
        };
        static_assert(sizeof(RelocEntry) == 0x18);

        struct RelocBucket {
            u32 _pad_;
            u32 numEntries;
            u64 endOffset;
            std::vector<RelocEntry> entries;
        };

        struct RelocTree {
            u32 _pad_;
            u32 numBuckets;
            u64 totalSize;
            std::vector<u64> bucketVirtualOffsets;
            std::vector<RelocBucket> buckets;
        };

        struct SubsecEntry {
            u64 offset;
            u32 _pad_;
            u32 ctrVal;
        };
        static_assert(sizeof(SubsecEntry) == 0x10);

        struct SubsecBucket {
            u32 _pad_;
            u32 numEntries;
            u64 endOffset;
            std::vector<SubsecEntry> entries;
        };

        struct SubsecTree {
            u32 _pad_;
            u32 numBuckets;
            u64 totalSize;
            std::vector<u64> bucketPhysicalOffsets;
            std::vector<SubsecBucket> buckets;
        };

        //!< Result of a relocation lookup: the entry itself plus the offset where its region ends
        struct RelocLookup {
            RelocEntry entry;
            size_t regionEnd; //!< Virtual offset at which the next entry's region begins
        };

        //!< Result of a subsection lookup: the entry itself plus the offset where its region ends
        struct SubsecLookup {
            SubsecEntry entry;
            size_t regionEnd; //!< Physical offset at which the next entry's region begins
        };

        std::shared_ptr<Backing> rawBacking;
        std::shared_ptr<Backing> baseBacking;
        crypto::KeyStore::Key128 key{};
        u32 secureValue{};
        u32 generation{};
        size_t sectionBaseOffset{};
        RelocTree relocation;
        SubsecTree subsections;
        bool hasRelocation{false};
        bool hasSubsections{false};

        static std::array<u8, 0x10> MakeCtr(u32 secureValue, u32 generation);
        static std::array<u8, 0x10> MakeBktrCtr(const std::array<u8, 0x10> &baseCtr, u32 ctrVal);

        template<typename T>
        static T ReadLe(const u8 *ptr) {
            T value{};
            std::memcpy(&value, ptr, sizeof(T));
            return value;
        }

        void LoadTables(size_t relocationOffset, size_t relocationSize, size_t subsectionOffset, size_t subsectionSize);

        //!< Finds the index of the last bucket whose start offset is <= target, via binary search over the sorted L1 index
        static u32 FindBucketIndex(const std::vector<u64> &bucketOffsets, size_t target);

        RelocLookup FindRelocation(size_t offset);
        SubsecLookup FindSubsection(size_t offset);
        size_t ReadPhysical(span<u8> output, size_t physicalOffset, u32 ctrVal, bool useBktrCtr);
        size_t ReadPatched(span<u8> output, size_t offset);

      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override;

      public:
        BktrBacking(crypto::KeyStore::Key128 key,
                    u32 secureValue,
                    u32 generation,
                    std::shared_ptr<Backing> rawBacking,
                    size_t sectionBaseOffset,
                    size_t relocationOffset,
                    size_t relocationSize,
                    size_t subsectionOffset,
                    size_t subsectionSize,
                    std::shared_ptr<Backing> baseBacking = nullptr);
    };
}
