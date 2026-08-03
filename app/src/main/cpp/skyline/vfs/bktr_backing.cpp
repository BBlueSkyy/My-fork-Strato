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
            if (ReadLe<u32>(ptr + 0x0) != 0x52445442) { // "BKTR"
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
                bucket.numEntries = ReadLe<u32>(ptr + bucketBase + 0x4);
                bucket.endOffset = ReadLe<u64>(ptr + bucketBase + 0x8);
                bucket.entries.resize(static_cast<size_t>(bucket.numEntries) + 1);

                for (size_t e = 0; e <= bucket.numEntries; ++e) {
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
            if (ReadLe<u32>(ptr + 0x0) != 0x52445442) { // "BKTR"
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
                bucket.numEntries = ReadLe<u32>(ptr + bucketBase + 0x4);
                bucket.endOffset = ReadLe<u64>(ptr + bucketBase + 0x8);
                bucket.entries.resize(static_cast<size_t>(bucket.numEntries) + 1);

                for (size_t e = 0; e <= bucket.numEntries; ++e) {
                    size_t entryBase{bucketBase + 0x10 + e * sizeof(SubsecEntry)};
                    bucket.entries[e].offset = ReadLe<u64>(ptr + entryBase + 0x0);
                    bucket.entries[e]._pad_ = ReadLe<u32>(ptr + entryBase + 0x8);
                    bucket.entries[e].ctrVal = ReadLe<u32>(ptr + entryBase + 0xC);
                }
            }

            hasSubsections = true;
        }
    }

    BktrBacking::RelocEntry &BktrBacking::GetRelocation(size_t offset) {
        if (!hasRelocation)
            throw exception("BKTR relocation table not loaded");

        if (offset > relocation.totalSize)
            throw exception("BKTR relocation offset out of range");

        u32 bucketNum{0};
        for (u32 i = 1; i < relocation.numBuckets; ++i) {
            if (relocation.bucketVirtualOffsets[i] <= offset)
                ++bucketNum;
        }

        auto &bucket{relocation.buckets.at(bucketNum)};
        if (bucket.numEntries == 1)
            return bucket.entries[0];

        u32 low{0}, high{bucket.numEntries - 1};
        while (low <= high) {
            u32 mid{(low + high) / 2};
            if (bucket.entries[mid].virtOffset > offset) {
                high = mid - 1;
            } else {
                if (mid == bucket.numEntries - 1 || bucket.entries[mid + 1].virtOffset > offset)
                    return bucket.entries[mid];
                low = mid + 1;
            }
        }

        throw exception("Failed to resolve BKTR relocation entry");
    }

    BktrBacking::SubsecEntry &BktrBacking::GetSubsection(size_t offset) {
        if (!hasSubsections)
            throw exception("BKTR subsection table not loaded");

        auto &lastBucket{subsections.buckets.back()};
        if (offset >= lastBucket.entries[lastBucket.numEntries].offset)
            return lastBucket.entries[lastBucket.numEntries];

        u32 bucketNum{0};
        for (u32 i = 1; i < subsections.numBuckets; ++i) {
            if (subsections.bucketPhysicalOffsets[i] <= offset)
                ++bucketNum;
        }

        auto &bucket{subsections.buckets.at(bucketNum)};
        if (bucket.numEntries == 1)
            return bucket.entries[0];

        u32 low{0}, high{bucket.numEntries - 1};
        while (low <= high) {
            u32 mid{(low + high) / 2};
            if (bucket.entries[mid].offset > offset) {
                high = mid - 1;
            } else {
                if (mid == bucket.numEntries - 1 || bucket.entries[mid + 1].offset > offset)
                    return bucket.entries[mid];
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
            auto &reloc{GetRelocation(offset + totalRead)};
            size_t virtOffset{offset + totalRead};
            size_t virtBoundary{reloc.virtOffset + 0xFFFFFFFFFFFFFFFFULL};

            if (reloc.isPatch == 0) {
                if (!baseBacking) {
                    size_t chunk{std::min(output.size() - totalRead, subsections.totalSize > virtOffset ? subsections.totalSize - virtOffset : size_t{0})};
                    if (chunk == 0)
                        break;
                    std::memset(output.data() + totalRead, 0, chunk);
                    totalRead += chunk;
                    continue;
                }

                size_t mappedOffset{virtOffset - reloc.virtOffset + reloc.physOffset};
                size_t nextVirt{relocation.totalSize};

                if (reloc.virtOffset < relocation.totalSize) {
                    auto &bucket{GetRelocation(virtOffset)};
                    (void)bucket;
                }

                size_t chunk{output.size() - totalRead};
                if (baseBacking->size < mappedOffset)
                    chunk = 0;
                else
                    chunk = std::min(chunk, baseBacking->size - mappedOffset);

                if (chunk == 0)
                    break;

                baseBacking->ReadUnchecked(output.subspan(totalRead, chunk), mappedOffset);
                totalRead += chunk;
                continue;
            }

            size_t physOffset{virtOffset - reloc.virtOffset + reloc.physOffset};
            auto &subsec{GetSubsection(physOffset)};
            size_t nextBoundary{subsections.totalSize};

            auto &bucket{subsections.buckets.back()};
            if (subsec.offset != bucket.entries[bucket.numEntries].offset) {
                if (&subsec != &bucket.entries[bucket.numEntries]) {
                    auto &bucketRef{GetSubsection(physOffset)};
                    (void)bucketRef;
                }
            }

            size_t chunk{output.size() - totalRead};
            if (physOffset < subsections.totalSize)
                chunk = std::min(chunk, subsections.totalSize - physOffset);

            if (&subsec != &bucket.entries[bucket.numEntries]) {
                size_t end{subsec.offset};
                auto &curBucket{GetSubsection(physOffset)};
                (void)curBucket;
            }

            if (chunk == 0)
                break;

            size_t subsecBoundary{subsections.totalSize};
            {
                auto &lastBucket{subsections.buckets.back()};
                if (physOffset >= lastBucket.entries[lastBucket.numEntries].offset) {
                    subsecBoundary = subsections.totalSize;
                } else {
                    auto &entry{GetSubsection(physOffset)};
                    auto &bucketForOffset{entry};
                    (void)bucketForOffset;
                }
            }

            auto &entry{GetSubsection(physOffset)};
            size_t nextPhysBoundary{subsections.totalSize};

            {
                u32 bucketNum{0};
                for (u32 i = 1; i < subsections.numBuckets; ++i) {
                    if (subsections.bucketPhysicalOffsets[i] <= physOffset)
                        ++bucketNum;
                }
                auto &bucket{subsections.buckets.at(bucketNum)};
                if (&entry != &bucket.entries[bucket.numEntries]) {
                    for (u32 e = 0; e < bucket.numEntries; ++e) {
                        if (bucket.entries[e].offset == entry.offset) {
                            nextPhysBoundary = bucket.entries[e + 1].offset;
                            break;
                        }
                    }
                }
            }

            chunk = std::min(chunk, nextPhysBoundary > physOffset ? nextPhysBoundary - physOffset : size_t{0});
            if (chunk == 0)
                break;

            ReadPhysical(output.subspan(totalRead, chunk), physOffset, entry.ctrVal, true);
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
            auto &subsec{GetSubsection(offset)};
            return ReadPhysical(output, offset, subsec.ctrVal, true);
        }

        return ReadPatched(output, offset);
    }
}
