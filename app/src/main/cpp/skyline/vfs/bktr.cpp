// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include "bktr.h"
#include "region_backing.h"

namespace skyline::vfs {
    template <typename BlockType, typename BucketType>
    std::pair<u64, u64> SearchBucketEntry(u64 offset, const BlockType &block, const BucketType &buckets, bool isSubsection) {
        if (block.numberBuckets == 0 || block.numberBuckets > block.baseOffsets.size() || block.numberBuckets > buckets.size())
            throw exception("BKTR: invalid bucket count {}", block.numberBuckets);

        if (isSubsection) {
            const auto &lastBucket{buckets[block.numberBuckets - 1]};
            if (lastBucket.numberEntries >= lastBucket.entries.size())
                throw exception("BKTR: subsection bucket is missing its boundary entry");
            if (offset >= lastBucket.entries[lastBucket.numberEntries].addressPatch) {
                return {block.numberBuckets - 1, lastBucket.numberEntries};
            }
        }

        auto bucketIt{std::upper_bound(block.baseOffsets.begin(), block.baseOffsets.begin() + block.numberBuckets, offset)};
        if (bucketIt == block.baseOffsets.begin())
            throw exception("BKTR: offset 0x{:X} is before the first bucket", offset);
        const u64 bucketId{static_cast<u64>(std::distance(block.baseOffsets.begin(), bucketIt) - 1)};

        const auto &bucket{buckets[bucketId]};
        if (bucket.numberEntries == 0 || bucket.numberEntries > bucket.entries.size())
            throw exception("BKTR: invalid entry count {} in bucket {}", bucket.numberEntries, bucketId);

        if (bucket.numberEntries == 1)
            return {bucketId, 0};

        auto entryIt{std::upper_bound(bucket.entries.begin(), bucket.entries.begin() + bucket.numberEntries, offset, [](u64 offset, const auto &entry) {
            return offset < entry.addressPatch;
        })};

        if (entryIt != bucket.entries.begin()) {
            u64 entryIndex{static_cast<u64>(std::distance(bucket.entries.begin(), entryIt) - 1)};
            return {bucketId, entryIndex};
        }
        throw exception("BKTR: offset 0x{:X} could not be found", offset);
    }

    BKTR::BKTR(std::shared_ptr<vfs::Backing> pBaseRomfs, std::shared_ptr<vfs::Backing> pBktrRomfs, RelocationBlock pRelocation,
               std::vector<RelocationBucket> pRelocationBuckets, SubsectionBlock pSubsection,
               std::vector<SubsectionBucket> pSubsectionBuckets, bool pIsEncrypted, std::array<u8, 16> pKey,
               u64 pBaseOffset, u64 pIvfcOffset, std::array<u8, 8> pSectionCtr)
               : Backing({true, false, false}, pRelocation.size), baseRomFs(std::move(pBaseRomfs)), bktrRomFs(std::move(pBktrRomfs)),
                 relocation(pRelocation), relocationBuckets(std::move(pRelocationBuckets)),
                 subsection(pSubsection), subsectionBuckets(std::move(pSubsectionBuckets)),
                 isEncrypted(pIsEncrypted), key(pKey), baseOffset(pBaseOffset), ivfcOffset(pIvfcOffset),
                 sectionCtr(pSectionCtr) {

        if (!baseRomFs || !bktrRomFs)
            throw exception("BKTR: missing base or patch backing");
        if (relocation.index != 0 || subsection.index != 0 || relocation.numberBuckets == 0 || subsection.numberBuckets == 0 ||
            relocation.numberBuckets != relocationBuckets.size() || subsection.numberBuckets != subsectionBuckets.size() ||
            relocation.numberBuckets > relocation.baseOffsets.size() || subsection.numberBuckets > subsection.baseOffsets.size())
            throw exception("BKTR: invalid root nodes");

        for (size_t i{}; i < relocationBuckets.size(); ++i) {
            if (relocationBuckets[i].numberEntries == 0 || relocationBuckets[i].numberEntries > relocationBuckets[i].entries.size())
                throw exception("BKTR: invalid relocation bucket {}", i);
        }
        for (size_t i{}; i < subsectionBuckets.size(); ++i) {
            if (subsectionBuckets[i].numberEntries == 0 || subsectionBuckets[i].numberEntries > subsectionBuckets[i].entries.size())
                throw exception("BKTR: invalid subsection bucket {}", i);
        }

        for (std::size_t i = 0; i < relocation.numberBuckets - 1; ++i)
            relocationBuckets[i].entries.push_back({relocation.baseOffsets[i + 1], 0, 0});

        for (std::size_t i = 0; i < subsection.numberBuckets - 1; ++i)
            subsectionBuckets[i].entries.push_back({subsectionBuckets[i + 1].entries[0].addressPatch, {0}, subsectionBuckets[i + 1].entries[0].ctr});

        relocationBuckets.back().entries.push_back({relocation.size, 0, 0});
    }

    size_t BKTR::ReadImpl(span<u8> output, size_t offset) {
        if (output.empty())
            return 0;

        if (offset >= relocation.size)
            return 0;

        const auto relocationEntry{GetRelocationEntry(offset)};
        const auto sectionOffset{offset - relocationEntry.addressPatch + relocationEntry.addressSource};

        const auto nextRelocation{GetNextRelocationEntry(offset)};

       if (nextRelocation.addressPatch <= offset)
           throw exception("BKTR::ReadImpl: non-advancing relocation boundary at offset 0x{:X}", offset);

       if (output.size() > nextRelocation.addressPatch - offset) {
           const u64 partition{nextRelocation.addressPatch - offset};
           span<u8> data(output.data() + partition, output.size() - partition);
           return ReadWithPartition(data, output.size() - partition, offset + partition) + ReadWithPartition(output, partition, offset);
       }

       if (!relocationEntry.fromPatch) {
           if (sectionOffset < ivfcOffset || sectionOffset - ivfcOffset > baseRomFs->size ||
               output.size() > baseRomFs->size - (sectionOffset - ivfcOffset))
               throw exception("BKTR: base read is outside the base RomFS");
           auto regionBacking{std::make_shared<RegionBacking>(baseRomFs, sectionOffset - ivfcOffset, output.size())};
           return regionBacking->Read(output);
       }

        if (sectionOffset > bktrRomFs->size || output.size() > bktrRomFs->size - sectionOffset)
            throw exception("BKTR: patch read is outside the update section");

        if (!isEncrypted)
            return bktrRomFs->Read(output, sectionOffset);

        const auto subsectionEntry{GetSubsectionEntry(sectionOffset)};

        crypto::AesCipher cipher(key, MBEDTLS_CIPHER_AES_128_CTR);
        cipher.SetIV(GetCipherIV(subsectionEntry, sectionOffset));

        const auto nextSubsection{GetNextSubsectionEntry(sectionOffset)};

        if (nextSubsection.addressPatch <= sectionOffset)
            throw exception("BKTR::ReadImpl: non-advancing subsection boundary at offset 0x{:X}", sectionOffset);

        if (output.size() > nextSubsection.addressPatch - sectionOffset) {
            const u64 partition{nextSubsection.addressPatch - sectionOffset};
            span<u8> data(output.data() + partition, output.size() - partition);
            return ReadWithPartition(data, output.size() - partition, offset + partition) +
                ReadWithPartition(output, partition, offset);
        }

        const auto blockOffset{sectionOffset & 0xF};
        if (blockOffset != 0) {
            std::vector<u8> block(0x10);
            auto regionBacking{std::make_shared<RegionBacking>(bktrRomFs, sectionOffset & static_cast<u32>(~0xF), 0x10)};
            regionBacking->Read(block);

            cipher.Decrypt(block.data(), block.data(), block.size());
            if (output.size() + blockOffset < 0x10) {
                std::memcpy(output.data(), block.data() + blockOffset, std::min(output.size(), block.size()));
                return std::min(output.size(), block.size());
            }

            const auto read{0x10 - blockOffset};
            std::memcpy(output.data(), block.data() + blockOffset, read);
            span<u8> data(output.data() + read, output.size() - read);
            return read + ReadWithPartition(data, output.size() - read, offset + read);
        }

        auto regionBacking{std::make_shared<RegionBacking>(bktrRomFs, sectionOffset, output.size())};
        auto readSize{regionBacking->Read(output)};
        cipher.Decrypt(output.data(), output.data(), readSize);
        return readSize;
    }

    size_t BKTR::ReadWithPartition(span<u8> output, size_t length, size_t offset) {
        if (length == 0)
            return 0;
        if (length > output.size())
            throw exception("BKTR::ReadWithPartition: length exceeds output span");

        if (offset >= relocation.size)
            return 0;

        const auto relocationEntry{GetRelocationEntry(offset)};
        const auto sectionOffset{offset - relocationEntry.addressPatch + relocationEntry.addressSource};

        const auto nextRelocation{GetNextRelocationEntry(offset)};

        if (nextRelocation.addressPatch <= offset)
            throw exception("BKTR::ReadWithPartition: non-advancing relocation boundary at offset 0x{:X}", offset);

        if (length > nextRelocation.addressPatch - offset) {
            const u64 partition{nextRelocation.addressPatch - offset};
            span<u8> data(output.data() + partition, length - partition);
            return ReadWithPartition(data, length - partition, offset + partition) + ReadWithPartition(output, partition, offset);
        }

       if (!relocationEntry.fromPatch) {
           if (sectionOffset < ivfcOffset || sectionOffset - ivfcOffset > baseRomFs->size ||
               length > baseRomFs->size - (sectionOffset - ivfcOffset))
               throw exception("BKTR: base read is outside the base RomFS");
           span<u8> data(output.data(), length);
           auto regionBacking{std::make_shared<RegionBacking>(baseRomFs, sectionOffset - ivfcOffset, length)};
           return regionBacking->Read(data);
       }

       if (sectionOffset > bktrRomFs->size || length > bktrRomFs->size - sectionOffset)
           throw exception("BKTR: patch read is outside the update section");

       if (!isEncrypted)
           return bktrRomFs->Read(output, sectionOffset);

        const auto subsectionEntry{GetSubsectionEntry(sectionOffset)};

        crypto::AesCipher cipher(key, MBEDTLS_CIPHER_AES_128_CTR);
        cipher.SetIV(GetCipherIV(subsectionEntry, sectionOffset));

        const auto nextSubsection{GetNextSubsectionEntry(sectionOffset)};

        if (nextSubsection.addressPatch <= sectionOffset)
            throw exception("BKTR::ReadWithPartition: non-advancing subsection boundary at offset 0x{:X}", sectionOffset);

        if (length > nextSubsection.addressPatch - sectionOffset) {
            const u64 partition{nextSubsection.addressPatch - sectionOffset};
            span<u8> data(output.data() + partition, length - partition);
            return ReadWithPartition(data, length - partition, offset + partition) +
                ReadWithPartition(output, partition, offset);
        }

        const auto blockOffset{sectionOffset & 0xF};
        if (blockOffset != 0) {
            std::vector<u8> block(0x10);
            auto regionBacking{std::make_shared<RegionBacking>(bktrRomFs, sectionOffset & static_cast<u32>(~0xF), 0x10)};
            regionBacking->Read(block);

            cipher.Decrypt(block.data(), block.data(), block.size());
            if (length + blockOffset < 0x10) {
                std::memcpy(output.data(), block.data() + blockOffset, std::min(length, block.size()));
                return std::min(length, block.size());
            }

            const auto read{0x10 - blockOffset};
            std::memcpy(output.data(), block.data() + blockOffset, read);
            span<u8> data(output.data() + read, length - read);
            return read + ReadWithPartition(data, length - read, offset + read);
        }

        auto regionBacking{std::make_shared<RegionBacking>(bktrRomFs, sectionOffset, length)};
        span<u8> data(output.data(), length);
        size_t readSize{0};
        if (length)
            readSize = regionBacking->Read(data);
        cipher.Decrypt(data.data(), data.data(), readSize);
        return readSize;
    }

    SubsectionEntry BKTR::GetNextSubsectionEntry(u64 offset) {
        const auto entry{SearchBucketEntry(offset, subsection, subsectionBuckets, true)};
        const auto bucket{subsectionBuckets[entry.first]};
        if (entry.second + 1 < bucket.entries.size())
            return bucket.entries[entry.second + 1];
        if (entry.first + 1 >= subsectionBuckets.size() || subsectionBuckets[entry.first + 1].entries.empty())
            throw exception("BKTR: missing next subsection entry");
        return subsectionBuckets[entry.first + 1].entries.front();
    }

    RelocationEntry BKTR::GetRelocationEntry(u64 offset) {
        const auto entry{SearchBucketEntry(offset, relocation, relocationBuckets, false)};
        return relocationBuckets[entry.first].entries[entry.second];
    }

    SubsectionEntry BKTR::GetSubsectionEntry(u64 offset) {
        const auto entry{SearchBucketEntry(offset, subsection, subsectionBuckets, true)};
        return subsectionBuckets[entry.first].entries[entry.second];
    }

    RelocationEntry BKTR::GetNextRelocationEntry(u64 offset) {
        const auto entry{SearchBucketEntry(offset, relocation, relocationBuckets, false)};
        const auto bucket{relocationBuckets[entry.first]};
        if (entry.second + 1 < bucket.entries.size())
            return bucket.entries[entry.second + 1];
        if (entry.first + 1 >= relocationBuckets.size() || relocationBuckets[entry.first + 1].entries.empty())
            throw exception("BKTR: missing next relocation entry");
        return relocationBuckets[entry.first + 1].entries.front();
    }

    std::array<u8, 16> BKTR::GetCipherIV(SubsectionEntry subsectionEntry, u64 sectionOffset) {
        std::array<u8, 16> iv{};
        auto subsectionCtr{subsectionEntry.ctr};
        auto offset_iv{sectionOffset + baseOffset};
        for (std::size_t i = 0; i < sectionCtr.size(); ++i) {
            iv[i] = sectionCtr[0x8 - i - 1];
        }
        offset_iv >>= 4;
        for (std::size_t i = 0; i < sizeof(u64); ++i) {
            iv[0xF - i] = static_cast<u8>(offset_iv & 0xFF);
            offset_iv >>= 8;
        }
        for (std::size_t i = 0; i < sizeof(u32); ++i) {
            iv[0x7 - i] = static_cast<u8>(subsectionCtr & 0xFF);
            subsectionCtr >>= 8;
        }
        return iv;
    }
}
