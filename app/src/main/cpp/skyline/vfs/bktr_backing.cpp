// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include <common.h>
#include <crypto/aes_cipher.h>
#include "bktr_backing.h"
#include "ctr_encrypted_backing.h"
#include "region_backing.h"

namespace skyline::vfs {
    static constexpr size_t BktrBucketSize{0x4000};
    static constexpr size_t BktrHeaderSize{0x4000};

    static std::shared_ptr<Backing> MakeEncryptedRegionBacking(
        const std::shared_ptr<Backing> &backing,
        const crypto::KeyStore::Key128 &key,
        const std::array<u8, 0x10> &ctr,
        size_t baseOffset
    ) {
        return std::make_shared<CtrEncryptedBacking>(ctr, key, backing, baseOffset);
    }

    std::array<u8, 0x10> BktrBacking::MakeCtr(u32 secureValue, u32 generation) {
        std::array<u8, 0x10> ctr{};
        u32 secureValueLE{util::SwapEndianness(secureValue)};
        u32 generationLE{util::SwapEndianness(generation)};
        std::memcpy(ctr.data(), &secureValueLE, 4);
        std::memcpy(ctr.data() + 4, &generationLE, 4);
        return ctr;
    }

    std::array<u8, 0x10> BktrBacking::MakeBktrCtr(const std::array<u8, 0x10> &baseCtr, u32 ctrVal) {
        auto ctr{baseCtr};
        for (unsigned int j = 0; j < 4; ++j)
            ctr[0x8 - j - 1] = static_cast<u8>((ctrVal >> (j * 8)) & 0xFF);
        return ctr;
    }

    BktrBacking::BktrBacking(crypto::KeyStore::Key128 key,
                             u32 secureValue,
                             u32 generation,
                             std::shared_ptr<Backing> rawBacking,
                             size_t sectionBaseOffset,
                             size_t relocationOffset,
                             size_t relocationSize,
                             size_t subsectionOffset,
                             size_t subsectionSize,
                             std::shared_ptr<Backing> baseBacking)
        : Backing({true, false, false}),
          rawBacking(std::move(rawBacking)),
          baseBacking(std::move(baseBacking)),
          key(key),
          secureValue(secureValue),
          generation(generation),
          sectionBaseOffset(sectionBaseOffset) {
        auto defaultCtr{MakeCtr(secureValue, generation)};
        LoadTables(relocationOffset, relocationSize, subsectionOffset, subsectionSize);

        if (hasRelocation && baseBacking)
            size = relocation.totalSize;
        else if (hasSubsections)
            size = subsections.totalSize;
        else
            size = this->rawBacking->size;
    }

    void BktrBacking::LoadTables(size_t relocationOffset, size_t relocationSize, size_t subsectionOffset, size_t subsectionSize) {
        auto defaultCtr{MakeCtr(secureValue, generation)};

        if (relocationSize) {
            auto tableRegion{std::make_shared<RegionBacking>(rawBacking, relocationOffset, relocationSize)};
            auto tableBacking{MakeEncryptedRegionBacking(tableRegion, key, defaultCtr, sectionBaseOffset + relocationOffset)};

            std::vector<u8> table(relocationSize);
            tableBacking->Read(span(table.data(), table.size()), 0);

            const u8 *ptr{table.data()};
            // 'BKTR' as little-endian u32 => 0x52544B42
            if (ReadLe<u32>(ptr + 0x0) != 0x52544B42) {
                throw exception("Invalid BKTR relocation table magic");
            }

            relocation.numBuckets = ReadLe<u32>(ptr + 0x4);
            relocation.totalSize = ReadLe<u64>(ptr + 0x8);
            relocation.bucketVirtualOffsets.resize(relocation.numBuckets);
            relocation.buckets.resize(relocation.numBuckets);

            for (u32 i = 0; i < relocation.numBuckets; ++i)
                relocation.bucketVirtualOffsets[i] = ReadLe<u64>(ptr + 0x10 + static_cast<size_t>(i) * 8);

            for (u32 i = 0; i < relocation.numBuckets; ++i) {
                size_t bucketBase{BktrHeaderSize + static_cast<size_t>(i) * BktrBucketSize};
                auto &bucket{relocation.buckets[i]};

                // Basic bounds checks to avoid OOB reads on truncated/corrupt tables
                if (bucketBase + 0x10 > table.size())
                    throw exception("BKTR relocation table truncated (bucket header out of range)");

                bucket.numEntries = ReadLe<u32>(ptr + bucketBase + 0x4);
                bucket.endOffset = ReadLe<u64>(ptr + bucketBase + 0x8);

                size_t entriesCountOnDisk = static_cast<size_t>(bucket.numEntries) + 1; // on-disk stores numEntries+1
                size_t entriesBytes = entriesCountOnDisk * sizeof(RelocEntry);
                if (bucketBase + 0x10 + entriesBytes > table.size())
                    throw exception("BKTR relocation table truncated (entries out of range)");

                bucket.entries.resize(entriesCountOnDisk);

                for (size_t e = 0; e < entriesCountOnDisk; ++e) {
                    size_t entryBase{bucketBase + 0x10 + e * sizeof(RelocEntry)};
                    bucket.entries[e].virtOffset = ReadLe<u64>(ptr + entryBase + 0x0);
                    bucket.entries[e].physOffset = ReadLe<u64>(ptr + entryBase + 0x8);
                    bucket.entries[e].isPatch = ReadLe<u32>(ptr + entryBase + 0x10);
                    bucket.entries[e]._pad_ = 0;
                }
            }

            hasRelocation = true;
        }

        if (subsectionSize) {
            auto tableRegion{std::make_shared<RegionBacking>(rawBacking, subsectionOffset, subsectionSize)};
            auto tableBacking{MakeEncryptedRegionBacking(tableRegion, key, defaultCtr, sectionBaseOffset + subsectionOffset)};

            std::vector<u8> table(subsectionSize);
            tableBacking->Read(span(table.data(), table.size()), 0);

            const u8 *ptr{table.data()};
            // 'BKTR' as little-endian u32 => 0x52544B42
            if (ReadLe<u32>(ptr + 0x0) != 0x52544B42) {
                throw exception("Invalid BKTR subsection table magic");
            }

            subsections.numBuckets = ReadLe<u32>(ptr + 0x4);
            subsections.totalSize = ReadLe<u64>(ptr + 0x8);
            subsections.bucketPhysicalOffsets.resize(subsections.numBuckets);
            subsections.buckets.resize(subsections.numBuckets);

            for (u32 i = 0; i < subsections.numBuckets; ++i)
                subsections.bucketPhysicalOffsets[i] = ReadLe<u64>(ptr + 0x10 + static_cast<size_t>(i) * 8);

            for (u32 i = 0; i < subsections.numBuckets; ++i) {
                size_t bucketBase{BktrHeaderSize + static_cast<size_t>(i) * BktrBucketSize};
                auto &bucket{subsections.buckets[i]};

                if (bucketBase + 0x10 > table.size())
                    throw exception("BKTR subsection table truncated (bucket header out of range)");

                bucket.numEntries = ReadLe<u32>(ptr + bucketBase + 0x4);
                bucket.endOffset = ReadLe<u64>(ptr + bucketBase + 0x8);

                size_t entriesCountOnDisk = static_cast<size_t>(bucket.numEntries) + 1;
                size_t entriesBytes = entriesCountOnDisk * sizeof(SubsecEntry);
                if (bucketBase + 0x10 + entriesBytes > table.size())
                    throw exception("BKTR subsection table truncated (entries out of range)");

                bucket.entries.resize(entriesCountOnDisk);

                for (size_t e = 0; e < entriesCountOnDisk; ++e) {
                    size_t entryBase{bucketBase + 0x10 + e * sizeof(SubsecEntry)};
                    bucket.entries[e].offset = ReadLe<u64>(ptr + entryBase + 0x0);
                    bucket.entries[e]._pad_ = ReadLe<u32>(ptr + entryBase + 0x8);
                    bucket.entries[e].ctrVal = ReadLe<u32>(ptr + entryBase + 0xC);
                }
            }

            hasSubsections = true;
        }
    }

    u32 BktrBacking::FindBucketIndex(const std::vector<u64> &bucketOffsets, size_t target) {
        // bucketOffsets is the sorted L1 index (bucketOffsets[0] == 0); find the last entry <= target
        auto it{std::upper_bound(bucketOffsets.begin(), bucketOffsets.end(), target)};
        return static_cast<u32>(std::distance(bucketOffsets.begin(), it) - 1);
    }

    BktrBacking::RelocLookup BktrBacking::FindRelocation(size_t offset) {
        if (!hasRelocation)
            throw exception("BKTR relocation table not loaded");
        if (offset >= relocation.totalSize)
            throw exception("BKTR relocation offset out of range");

        auto &bucket{relocation.buckets.at(FindBucketIndex(relocation.bucketVirtualOffsets, offset))};

        // Every bucket stores numEntries+1 real entries on disk, so entries[mid + 1] is always
        // a valid boundary marker - no special-casing needed for the last entry in a bucket.
        int64_t low{0}, high{static_cast<int64_t>(bucket.numEntries) - 1};
        while (low <= high) {
            int64_t mid{low + (high - low) / 2};
            auto &entry{bucket.entries[static_cast<size_t>(mid)]};
            if (entry.virtOffset > offset) {
                high = mid - 1;
            } else {
                auto &next{bucket.entries[static_cast<size_t>(mid) + 1]};
                if (next.virtOffset > offset)
                    return {entry, next.virtOffset};
                low = mid + 1;
            }
        }

        throw exception("Failed to resolve BKTR relocation entry");
    }

    BktrBacking::SubsecLookup BktrBacking::FindSubsection(size_t offset) {
        if (!hasSubsections)
            throw exception("BKTR subsection table not loaded");
        if (offset >= subsections.totalSize)
            throw exception("BKTR subsection offset out of range");

        auto &bucket{subsections.buckets.at(FindBucketIndex(subsections.bucketPhysicalOffsets, offset))};

        int64_t low{0}, high{static_cast<int64_t>(bucket.numEntries) - 1};
        while (low <= high) {
            int64_t mid{low + (high - low) / 2};
            auto &entry{bucket.entries[static_cast<size_t>(mid)]};
            if (entry.offset > offset) {
                high = mid - 1;
            } else {
                auto &next{bucket.entries[static_cast<size_t>(mid) + 1]};
                if (next.offset > offset)
                    return {entry, next.offset};
                low = mid + 1;
            }
        }

        throw exception("Failed to resolve BKTR subsection entry");
    }

    size_t BktrBacking::ReadPhysical(span<u8> output, size_t physicalOffset, u32 ctrVal, bool useBktrCtr) {
        auto ctrBase{MakeCtr(secureValue, generation)};
        if (useBktrCtr)
            ctrBase = MakeBktrCtr(ctrBase, ctrVal);

        auto encrypted{MakeEncryptedRegionBacking(rawBacking, key, ctrBase, sectionBaseOffset)};
        return encrypted->ReadUnchecked(output, physicalOffset);
    }

    size_t BktrBacking::ReadPatched(span<u8> output, size_t offset) {
        size_t totalRead{0};

        while (totalRead < output.size()) {
            size_t virtOffset{offset + totalRead};
            auto reloc{FindRelocation(virtOffset)};

            // Never read past the end of this relocation entry's own region - the next entry
            // (whether unpatched or patched) may need completely different handling.
            size_t chunk{std::min(output.size() - totalRead, reloc.regionEnd - virtOffset)};

            if (reloc.entry.isPatch == 0) {
                // Unpatched region: the data is unchanged from the base title's RomFs
                if (!baseBacking) {
                    std::memset(output.data() + totalRead, 0, chunk);
                    totalRead += chunk;
                    continue;
                }

                size_t mappedOffset{virtOffset - reloc.entry.virtOffset + reloc.entry.physOffset};
                if (baseBacking->size <= mappedOffset)
                    break; // Base title doesn't have data here, nothing more we can provide

                chunk = std::min(chunk, baseBacking->size - mappedOffset);
                baseBacking->ReadUnchecked(output.subspan(totalRead, chunk), mappedOffset);
                totalRead += chunk;
                continue;
            }

            // Patched region: the data lives in this NCA's own encrypted content. It may still
            // span multiple subsections (each with its own AES-CTR value), so clamp further.
            size_t physOffset{virtOffset - reloc.entry.virtOffset + reloc.entry.physOffset};
            auto subsec{FindSubsection(physOffset)};
            chunk = std::min(chunk, subsec.regionEnd - physOffset);

            ReadPhysical(output.subspan(totalRead, chunk), physOffset, subsec.entry.ctrVal, true);
            totalRead += chunk;
        }

        return totalRead;
    }

    size_t BktrBacking::ReadImpl(span<u8> output, size_t offset) {
        if (!hasRelocation) {
            std::memset(output.data(), 0, output.size());
            return output.size();
        }

        if (!baseBacking) {
            // Direct physical read (no virtual->physical indirection): still need to switch
            // AES-CTR value whenever we cross into the next subsection.
            size_t totalRead{0};
            while (totalRead < output.size()) {
                size_t physOffset{offset + totalRead};
                auto subsec{FindSubsection(physOffset)};
                size_t chunk{std::min(output.size() - totalRead, subsec.regionEnd - physOffset)};

                ReadPhysical(output.subspan(totalRead, chunk), physOffset, subsec.entry.ctrVal, true);
                totalRead += chunk;
            }
            return totalRead;
        }

        return ReadPatched(output, offset);
    }
}],