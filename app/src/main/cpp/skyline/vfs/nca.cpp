// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <crypto/aes_cipher.h>
#include <loader/loader.h>
#include <limits>

#include "ctr_encrypted_backing.h"
#include "region_backing.h"
#include "partition_filesystem.h"
#include "nca.h"
#include "rom_filesystem.h"
#include "bktr.h"
#include "directory.h"
#include "sparse_storage.h"
#include "compressed_storage.h"

namespace skyline::vfs {
    using namespace loader;

    namespace {
        constexpr size_t BucketNodeSize{0x4000};
        constexpr size_t BucketNodeHeaderSize{0x10};
        constexpr size_t BucketOffsetsPerNode{(BucketNodeSize - BucketNodeHeaderSize) / sizeof(u64)};

        constexpr size_t DivideUp(size_t value, size_t divisor) {
            return value / divisor + (value % divisor != 0);
        }

        size_t QueryCompressedEntryStorageSize(u32 entryCount) {
            constexpr size_t EntriesPerBucket{682};
            return DivideUp(entryCount, EntriesPerBucket) * BucketNodeSize;
        }

        size_t QuerySparseEntryStorageSize(u32 entryCount) {
            constexpr size_t EntriesPerBucket{0x332};
            return DivideUp(entryCount, EntriesPerBucket) * BucketNodeSize;
        }

        size_t QuerySubsectionEntryStorageSize(u32 entryCount) {
            constexpr size_t EntriesPerBucket{0x3FF};
            return DivideUp(entryCount, EntriesPerBucket) * BucketNodeSize;
        }

        size_t QuerySingleLevelNodeStorageSize(size_t entryStorageSize) {
            const size_t bucketCount{entryStorageSize / BucketNodeSize};
            if (bucketCount == 0)
                return 0;
            if (bucketCount > BucketOffsetsPerNode)
                throw exception("Multi-level NCA bucket trees are not supported ({} entry buckets)", bucketCount);
            return BucketNodeSize;
        }

        template<typename Block>
        void ValidateRootBlock(const Block &block, size_t expectedBuckets, const char *name) {
            if (block.index != 0 || block.numberBuckets == 0 || block.numberBuckets != expectedBuckets ||
                block.numberBuckets > block.baseOffsets.size() || block.size == 0 || block.baseOffsets[0] != 0)
                throw exception("Invalid {} root node (index={}, buckets={}, expected={}, size=0x{:X})",
                                name, block.index, block.numberBuckets, expectedBuckets, block.size);

            for (size_t i{1}; i < block.numberBuckets; ++i) {
                if (block.baseOffsets[i - 1] >= block.baseOffsets[i] || block.baseOffsets[i] >= block.size)
                    throw exception("Invalid {} root offsets at bucket {}", name, i);
            }
        }
    }

    NCA::NCA(std::shared_ptr<vfs::Backing> pBacking, std::shared_ptr<crypto::KeyStore> pKeyStore, bool pUseKeyArea, NCAParseMode parseMode)
        : backing(std::move(pBacking)), keyStore(std::move(pKeyStore)), useKeyArea(pUseKeyArea) {
        header = backing->Read<NCAHeader>();

        if (header.magic != util::MakeMagic<u32>("NCA3")) {
            if (!keyStore->headerKey)
                throw loader_exception(LoaderResult::MissingHeaderKey);

            crypto::AesCipher cipher(*keyStore->headerKey, MBEDTLS_CIPHER_AES_128_XTS);
            cipher.XtsDecrypt({reinterpret_cast<u8 *>(&header), sizeof(NCAHeader)}, 0, 0x200);

            // Check if decryption was successful
            if (header.magic != util::MakeMagic<u32>("NCA3"))
                throw loader_exception(LoaderResult::ParsingError);
            encrypted = true;
        }

        contentType = header.contentType;
        rightsIdEmpty = header.rightsId == crypto::KeyStore::Key128{};

        const std::size_t numberSections{static_cast<size_t>(std::ranges::count_if(header.sectionTables, [](const NCASectionTableEntry &entry) {
            return entry.mediaOffset > 0;
        }))};

        sections.resize(numberSections);
        const auto lengthSections{constant::SectionHeaderSize * numberSections};

        if (encrypted) {
            std::vector<u8> raw(lengthSections);

            backing->Read(raw, constant::SectionHeaderOffset);

            crypto::AesCipher cipher(*keyStore->headerKey, MBEDTLS_CIPHER_AES_128_XTS);
            cipher.XtsDecrypt(reinterpret_cast<u8 *>(sections.data()), reinterpret_cast<u8 *>(raw.data()), lengthSections, 2, constant::SectionHeaderSize);
        } else {
            for (size_t i{}; i < numberSections; ++i)
                sections[i] = backing->Read<NCASectionHeader>(constant::SectionHeaderOffset + i * constant::SectionHeaderSize);
        }

        if (parseMode == NCAParseMode::MetadataOnly && contentType != NCAContentType::Meta && contentType != NCAContentType::Control)
            return;

        for (std::size_t i = 0; i < sections.size(); ++i) {
            const auto &section = sections[i];

            ValidateNCA(section);

            if (section.raw.header.fsType == NcaSectionFsType::RomFs) {
                ReadRomFs(section, header.sectionTables[i]);
            } else if (section.raw.header.fsType == NcaSectionFsType::PFS0) {
                ReadPfs0(section, header.sectionTables[i]);
            }
        }
    }

    NCA::NCA(std::optional<vfs::NCA> updateNca, std::shared_ptr<crypto::KeyStore> pKeyStore, std::shared_ptr<vfs::Backing> bktrBaseRomfs,
             u64 bktrBaseIvfcOffset, bool pUseKeyArea)
        : keyStore(std::move(pKeyStore)), bktrBaseRomfs(std::move(bktrBaseRomfs)), bktrBaseIvfcOffset(bktrBaseIvfcOffset), useKeyArea(pUseKeyArea) {
        if (!updateNca)
            throw loader_exception(LoaderResult::ParsingError);

        header = updateNca->header;
        sections = std::move(updateNca->sections);
        encrypted = updateNca->encrypted;
        backing = std::move(updateNca->backing);
        contentType = header.contentType;
        rightsIdEmpty = header.rightsId == crypto::KeyStore::Key128{};

        for (std::size_t i = 0; i < sections.size(); ++i) {
            const auto &section = sections[i];

            ValidateNCA(section);

            if (section.raw.header.fsType == NcaSectionFsType::RomFs)
                ReadRomFs(section, header.sectionTables[i]);
        }
    }

    void NCA::ReadPfs0(const NCASectionHeader &section, const NCASectionTableEntry &entry) {
        const size_t sectionStart{static_cast<size_t>(entry.mediaOffset) * constant::MediaUnitSize};
        const size_t sectionSize{constant::MediaUnitSize * static_cast<size_t>(entry.mediaEndOffset - entry.mediaOffset)};

        std::shared_ptr<Backing> encryptedSection{std::make_shared<RegionBacking>(backing, sectionStart, sectionSize)};
        encryptedSection = CreateSparseBacking(section, encryptedSection);
        auto decryptedSection{CreateBacking(section, encryptedSection, sectionStart)};
        if (!decryptedSection || section.pfs0.pfs0HeaderOffset > decryptedSection->size)
            throw loader_exception(LoaderResult::ParsingError, "PFS0 offset is outside the section");

        const size_t pfsPhysicalSize{decryptedSection->size - section.pfs0.pfs0HeaderOffset};
        std::shared_ptr<Backing> pfsBacking{std::make_shared<RegionBacking>(decryptedSection, section.pfs0.pfs0HeaderOffset, pfsPhysicalSize)};
        pfsBacking = CreateCompressedBacking(section, pfsBacking, pfsPhysicalSize);
        auto pfs{std::make_shared<PartitionFileSystem>(pfsBacking)};

        if (contentType == NCAContentType::Program) {
            // An ExeFS must always contain an NPDM and a main NSO, whereas the logo section will always contain a logo and a startup movie
            if (pfs->FileExists("main") && pfs->FileExists("main.npdm"))
                exeFs = std::move(pfs);
            else if (pfs->FileExists("NintendoLogo.png") && pfs->FileExists("StartupMovie.gif"))
                logo = std::move(pfs);
        } else if (contentType == NCAContentType::Meta) {
            cnmt = std::move(pfs);
        }
    }

    void NCA::ReadRomFs(const NCASectionHeader &sectionHeader, const NCASectionTableEntry &entry) {
        const size_t baseOffset{entry.mediaOffset * constant::MediaUnitSize};
        const size_t sectionSize{constant::MediaUnitSize * static_cast<size_t>(entry.mediaEndOffset - entry.mediaOffset)};
        ivfcOffset = sectionHeader.romfs.ivfc.levels[constant::IvfcMaxLevel - 1].offset;
        const size_t romFsSize{sectionHeader.romfs.ivfc.levels[constant::IvfcMaxLevel - 1].size};

        std::shared_ptr<Backing> encryptedSection{std::make_shared<RegionBacking>(backing, baseOffset, sectionSize)};
        encryptedSection = CreateSparseBacking(sectionHeader, encryptedSection);
        auto decryptedSection{CreateBacking(sectionHeader, encryptedSection, baseOffset)};
        if (!decryptedSection)
            throw loader_exception(LoaderResult::ParsingError, "Unsupported RomFS encryption type");

        if (sectionHeader.raw.header.encryptionType != NcaSectionEncryptionType::BKTR) {
            if (ivfcOffset > decryptedSection->size || romFsSize > decryptedSection->size - ivfcOffset)
                throw loader_exception(LoaderResult::ParsingError, "RomFS IVFC data layer is outside the section");

            auto patchLayer{std::make_shared<RegionBacking>(decryptedSection, ivfcOffset, romFsSize)};
            rawRomFs = patchLayer;
            romFs = CreateCompressedBacking(sectionHeader, rawRomFs, rawRomFs->size);
            return;
        }

        // A patch NCA cannot expose its final RomFS until it is combined with the base title. Keep the
        // pre-compression layer available so NspLoader can retain the update NCA without interpreting
        // its compression table against an incomplete standalone view.
        if (!bktrBaseRomfs) {
            rawRomFs.reset();
            romFs = decryptedSection;
            return;
        }

        const auto &relocationInfo{sectionHeader.bktr.relocation};
        const auto &subsectionInfo{sectionHeader.bktr.subsection};
        if (relocationInfo.magic != util::MakeMagic<u32>("BKTR") || subsectionInfo.magic != util::MakeMagic<u32>("BKTR") ||
            relocationInfo.version > 1 || subsectionInfo.version > 1 ||
            relocationInfo.numberEntries == 0 || subsectionInfo.numberEntries == 0)
            throw loader_exception(LoaderResult::ParsingError, "Invalid BKTR table headers");

        const size_t relocationEntryStorageSize{QuerySparseEntryStorageSize(relocationInfo.numberEntries)};
        const size_t subsectionEntryStorageSize{QuerySubsectionEntryStorageSize(subsectionInfo.numberEntries)};
        size_t relocationNodeStorageSize;
        size_t subsectionNodeStorageSize;
        try {
            relocationNodeStorageSize = QuerySingleLevelNodeStorageSize(relocationEntryStorageSize);
            subsectionNodeStorageSize = QuerySingleLevelNodeStorageSize(subsectionEntryStorageSize);
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ParsingError, e.what());
        }

        if (relocationNodeStorageSize > relocationInfo.size ||
            relocationEntryStorageSize > relocationInfo.size - relocationNodeStorageSize ||
            subsectionNodeStorageSize > subsectionInfo.size ||
            subsectionEntryStorageSize > subsectionInfo.size - subsectionNodeStorageSize)
            throw loader_exception(LoaderResult::ParsingError, "Invalid BKTR table extents");

        const size_t relocationOffset{relocationInfo.offset};
        const size_t subsectionOffset{subsectionInfo.offset};
        if (relocationOffset > decryptedSection->size || relocationInfo.size > decryptedSection->size - relocationOffset ||
            subsectionOffset > decryptedSection->size || subsectionInfo.size > decryptedSection->size - subsectionOffset)
            throw loader_exception(LoaderResult::ParsingError, "BKTR tables are outside the update section");

        RelocationBlock relocationBlock{decryptedSection->Read<RelocationBlock>(relocationOffset)};
        SubsectionBlock subsectionBlock{decryptedSection->Read<SubsectionBlock>(subsectionOffset)};
        try {
            ValidateRootBlock(relocationBlock, relocationEntryStorageSize / BucketNodeSize, "BKTR relocation");
            ValidateRootBlock(subsectionBlock, subsectionEntryStorageSize / BucketNodeSize, "BKTR subsection");
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ParsingError, e.what());
        }

        std::vector<RelocationBucketRaw> relocationBucketsRaw(relocationBlock.numberBuckets);
        decryptedSection->Read<RelocationBucketRaw>(relocationBucketsRaw, relocationOffset + relocationNodeStorageSize);
        std::vector<SubsectionBucketRaw> subsectionBucketsRaw(subsectionBlock.numberBuckets);
        decryptedSection->Read<SubsectionBucketRaw>(subsectionBucketsRaw, subsectionOffset + subsectionNodeStorageSize);

        std::vector<RelocationBucket> relocationBuckets;
        relocationBuckets.reserve(relocationBucketsRaw.size());
        size_t relocationEntryCount{};
        for (size_t i{}; i < relocationBucketsRaw.size(); ++i) {
            const auto &rawBucket{relocationBucketsRaw[i]};
            const u64 expectedEnd{i + 1 < relocationBlock.numberBuckets ? relocationBlock.baseOffsets[i + 1] : relocationBlock.size};
            if (rawBucket.index != i || rawBucket.numberEntries == 0 || rawBucket.numberEntries > rawBucket.relocationEntries.size() ||
                rawBucket.endOffset != expectedEnd || rawBucket.relocationEntries[0].addressPatch != relocationBlock.baseOffsets[i])
                throw loader_exception(LoaderResult::ParsingError, "Invalid BKTR relocation bucket");
            for (size_t j{}; j < rawBucket.numberEntries; ++j) {
                const auto &entry{rawBucket.relocationEntries[j]};
                if (entry.fromPatch > 1 || entry.addressPatch >= expectedEnd ||
                    (j != 0 && rawBucket.relocationEntries[j - 1].addressPatch >= entry.addressPatch))
                    throw loader_exception(LoaderResult::ParsingError, "Invalid BKTR relocation entry");
            }
            relocationEntryCount += rawBucket.numberEntries;
            relocationBuckets.push_back(ConvertRelocationBucketRaw(rawBucket));
        }

        std::vector<SubsectionBucket> subsectionBuckets;
        subsectionBuckets.reserve(subsectionBucketsRaw.size());
        size_t subsectionEntryCount{};
        for (size_t i{}; i < subsectionBucketsRaw.size(); ++i) {
            const auto &rawBucket{subsectionBucketsRaw[i]};
            const u64 expectedEnd{i + 1 < subsectionBlock.numberBuckets ? subsectionBlock.baseOffsets[i + 1] : subsectionBlock.size};
            if (rawBucket.index != i || rawBucket.numberEntries == 0 || rawBucket.numberEntries > rawBucket.subsectionEntries.size() ||
                rawBucket.endOffset != expectedEnd || rawBucket.subsectionEntries[0].addressPatch != subsectionBlock.baseOffsets[i])
                throw loader_exception(LoaderResult::ParsingError, "Invalid BKTR subsection bucket");
            for (size_t j{}; j < rawBucket.numberEntries; ++j) {
                const auto &entry{rawBucket.subsectionEntries[j]};
                if (entry.addressPatch >= expectedEnd ||
                    (j != 0 && rawBucket.subsectionEntries[j - 1].addressPatch >= entry.addressPatch))
                    throw loader_exception(LoaderResult::ParsingError, "Invalid BKTR subsection entry");
            }
            subsectionEntryCount += rawBucket.numberEntries;
            subsectionBuckets.push_back(ConvertSubsectionBucketRaw(rawBucket));
        }

        if ((relocationInfo.numberEntries != 0 && relocationEntryCount != relocationInfo.numberEntries) ||
            (subsectionInfo.numberEntries != 0 && subsectionEntryCount != subsectionInfo.numberEntries))
            throw loader_exception(LoaderResult::ParsingError, "BKTR entry count does not match its FS header");

        u32 ctrLow;
        std::memcpy(&ctrLow, sectionHeader.raw.sectionCtr.data(), sizeof(ctrLow));
        subsectionBuckets.back().entries.push_back({relocationInfo.offset, {0}, ctrLow});
        subsectionBuckets.back().entries.push_back({sectionSize, {0}, 0});

        std::array<u8, 0x10> key{};
        if (encrypted)
            key = !(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.raw.header.encryptionType);
        auto bktr{std::make_shared<BKTR>(
            bktrBaseRomfs, encryptedSection, relocationBlock, std::move(relocationBuckets), subsectionBlock,
            std::move(subsectionBuckets), encrypted, key, baseOffset,
            bktrBaseIvfcOffset, sectionHeader.raw.sectionCtr)};

        if (ivfcOffset > relocationBlock.size || romFsSize > relocationBlock.size - ivfcOffset)
            throw loader_exception(LoaderResult::ParsingError, "Patched RomFS IVFC layer is outside the BKTR virtual size");

        rawRomFs = std::make_shared<RegionBacking>(bktr, ivfcOffset, romFsSize);
        romFs = CreateCompressedBacking(sectionHeader, rawRomFs, rawRomFs->size);
    }

    std::shared_ptr<Backing> NCA::CreateBacking(const NCASectionHeader &sectionHeader, std::shared_ptr<Backing> rawBacking, size_t offset) {
        if (!encrypted)
            return rawBacking;

        switch (sectionHeader.raw.header.encryptionType) {
            case NcaSectionEncryptionType::None:
                return rawBacking;
            case NcaSectionEncryptionType::CTR:
            case NcaSectionEncryptionType::BKTR: {
                auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.raw.header.encryptionType)};

                std::array<u8, 0x10> ctr{};
                for (std::size_t i = 0; i < 8; ++i) {
                    ctr[i] = sectionHeader.raw.sectionCtr[8 - i - 1];
                }

                return std::make_shared<CtrEncryptedBacking>(ctr, key, std::move(rawBacking), offset);
            }
            default:
                return nullptr;
        }
    }

    std::shared_ptr<Backing> NCA::CreateSparseBacking(const NCASectionHeader &sectionHeader, std::shared_ptr<Backing> encryptedBacking) {
        const auto &sparseInfo{sectionHeader.raw.sparseInfo};
        if (sparseInfo.generation == 0)
            return encryptedBacking;

        const auto &tableHeader{sparseInfo.bucket.tableHeader};
        if (tableHeader.magic != util::MakeMagic<u32>("BKTR") || tableHeader.version > 1)
            throw loader_exception(LoaderResult::ErrorSparseNCA, "Invalid sparse BucketTree header");
        if (tableHeader.entryCount == 0)
            return std::make_shared<SparseStorage>(encryptedBacking->size);
        if (sparseInfo.bucket.tableOffset == 0 || sparseInfo.bucket.tableSize == 0)
            throw loader_exception(LoaderResult::ErrorSparseNCA, "Sparse BucketTree table is missing");

        std::array<u8, 0x10> tableCtr{};
        for (std::size_t i{}; i < 4; ++i)
            tableCtr[i] = sectionHeader.raw.sectionCtr[7 - i];

        const u32 sparseGen{static_cast<u32>(sparseInfo.generation) << 16};
        tableCtr[4] = static_cast<u8>((sparseGen >> 24) & 0xFF);
        tableCtr[5] = static_cast<u8>((sparseGen >> 16) & 0xFF);
        tableCtr[6] = 0;
        tableCtr[7] = 0;

        const size_t entryStorageSize{QuerySparseEntryStorageSize(tableHeader.entryCount)};
        size_t nodeStorageSize;
        try {
            nodeStorageSize = QuerySingleLevelNodeStorageSize(entryStorageSize);
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ErrorSparseNCA, e.what());
        }
        if (nodeStorageSize == 0 || nodeStorageSize > sparseInfo.bucket.tableSize ||
            entryStorageSize > sparseInfo.bucket.tableSize - nodeStorageSize)
            throw loader_exception(LoaderResult::ErrorSparseNCA, "Sparse BucketTree does not fit its table");

        const u64 physicalBase{sparseInfo.physicalOffset};
        if (physicalBase > backing->size || sparseInfo.bucket.tableOffset > backing->size - physicalBase ||
            sparseInfo.bucket.tableSize > backing->size - physicalBase - sparseInfo.bucket.tableOffset)
            throw loader_exception(LoaderResult::ErrorSparseNCA, "Sparse table is outside the NCA");

        std::shared_ptr<Backing> tableBacking{std::make_shared<RegionBacking>(
            backing, physicalBase, sparseInfo.bucket.tableOffset + sparseInfo.bucket.tableSize)};
        if (encrypted) {
            auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.raw.header.encryptionType)};
            tableBacking = std::make_shared<CtrEncryptedBacking>(tableCtr, key, tableBacking, physicalBase);
        }

        const size_t nodeStorageOffset{sparseInfo.bucket.tableOffset};
        RelocationBlock sparseBlock{tableBacking->Read<RelocationBlock>(nodeStorageOffset)};
        const size_t expectedBuckets{entryStorageSize / BucketNodeSize};
        try {
            ValidateRootBlock(sparseBlock, expectedBuckets, "sparse");
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ErrorSparseNCA, e.what());
        }

        std::vector<RelocationBucketRaw> sparseBucketsRaw(sparseBlock.numberBuckets);
        tableBacking->Read<RelocationBucketRaw>(sparseBucketsRaw, nodeStorageOffset + nodeStorageSize);

        std::vector<RelocationBucket> sparseBuckets;
        sparseBuckets.reserve(sparseBucketsRaw.size());
        size_t totalEntries{};
        for (size_t i{}; i < sparseBucketsRaw.size(); ++i) {
            const auto &rawBucket{sparseBucketsRaw[i]};
            const u64 expectedEnd{i + 1 < sparseBlock.numberBuckets ? sparseBlock.baseOffsets[i + 1] : sparseBlock.size};
            if (rawBucket.index != i || rawBucket.numberEntries == 0 || rawBucket.numberEntries > rawBucket.relocationEntries.size() ||
                rawBucket.endOffset != expectedEnd || rawBucket.relocationEntries[0].addressPatch != sparseBlock.baseOffsets[i])
                throw loader_exception(LoaderResult::ErrorSparseNCA, "Invalid sparse entry bucket");
            for (size_t j{}; j < rawBucket.numberEntries; ++j) {
                const auto &entry{rawBucket.relocationEntries[j]};
                if (entry.fromPatch > 1 || entry.addressPatch >= expectedEnd ||
                    (j != 0 && rawBucket.relocationEntries[j - 1].addressPatch >= entry.addressPatch))
                    throw loader_exception(LoaderResult::ErrorSparseNCA, "Invalid sparse entry");
            }
            totalEntries += rawBucket.numberEntries;
            sparseBuckets.push_back(ConvertRelocationBucketRaw(rawBucket));
        }
        if (totalEntries != tableHeader.entryCount)
            throw loader_exception(LoaderResult::ErrorSparseNCA, "Sparse entry count does not match its header");

        try {
            return std::make_shared<SparseStorage>(backing, sparseBlock, std::move(sparseBuckets), sparseBlock.size, physicalBase);
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ErrorSparseNCA, e.what());
        }
    }

    std::shared_ptr<Backing> NCA::CreateCompressedBacking(const NCASectionHeader &sectionHeader, std::shared_ptr<Backing> decryptedBacking, size_t virtualSize) {
        const auto &compressionInfo{sectionHeader.raw.compressionInfo};
        if (compressionInfo.bucket.tableOffset == 0 || compressionInfo.bucket.tableSize == 0)
            return decryptedBacking;

        const auto &tableHeader{compressionInfo.bucket.tableHeader};
        if (tableHeader.magic != util::MakeMagic<u32>("BKTR") || tableHeader.version > 1 || tableHeader.entryCount == 0)
            throw loader_exception(LoaderResult::ErrorCompressedNCA, "Invalid compressed BucketTree header");

        const size_t entryStorageSize{QueryCompressedEntryStorageSize(tableHeader.entryCount)};
        size_t nodeStorageSize;
        try {
            nodeStorageSize = QuerySingleLevelNodeStorageSize(entryStorageSize);
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ErrorCompressedNCA, e.what());
        }

        if (nodeStorageSize == 0 || nodeStorageSize > compressionInfo.bucket.tableSize ||
            entryStorageSize > compressionInfo.bucket.tableSize - nodeStorageSize)
            throw loader_exception(LoaderResult::ErrorCompressedNCA, "Compressed BucketTree does not fit its table");
        if (compressionInfo.bucket.tableOffset > decryptedBacking->size ||
            compressionInfo.bucket.tableSize > decryptedBacking->size - compressionInfo.bucket.tableOffset)
            throw loader_exception(LoaderResult::ErrorCompressedNCA, "Compressed table is outside the backing");

        const size_t nodeStorageOffset{compressionInfo.bucket.tableOffset};
        RelocationBlock compressedBlock{decryptedBacking->Read<RelocationBlock>(nodeStorageOffset)};
        const size_t expectedBuckets{entryStorageSize / BucketNodeSize};
        try {
            ValidateRootBlock(compressedBlock, expectedBuckets, "compressed");
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ErrorCompressedNCA, e.what());
        }

        std::vector<CompressedBucketRaw> compressedBucketsRaw(compressedBlock.numberBuckets);
        decryptedBacking->Read<CompressedBucketRaw>(compressedBucketsRaw, nodeStorageOffset + nodeStorageSize);

        std::vector<CompressedBucket> compressedBuckets;
        compressedBuckets.reserve(compressedBucketsRaw.size());
        size_t totalEntries{};
        u64 previousVirtualOffset{};
        bool firstEntry{true};
        for (size_t i{}; i < compressedBucketsRaw.size(); ++i) {
            const auto &rawBucket{compressedBucketsRaw[i]};
            const u64 expectedEnd{i + 1 < compressedBlock.numberBuckets ? compressedBlock.baseOffsets[i + 1] : compressedBlock.size};
            if (rawBucket.index != i || rawBucket.numberEntries == 0 || rawBucket.numberEntries > rawBucket.entries.size() ||
                rawBucket.endOffset != expectedEnd || rawBucket.entries[0].virtualOffset != compressedBlock.baseOffsets[i])
                throw loader_exception(LoaderResult::ErrorCompressedNCA, "Invalid compressed entry bucket");

            totalEntries += rawBucket.numberEntries;
            for (size_t j{}; j < rawBucket.numberEntries; ++j) {
                const auto &entry{rawBucket.entries[j]};
                if ((!firstEntry && entry.virtualOffset <= previousVirtualOffset) || entry.virtualOffset >= expectedEnd)
                    throw loader_exception(LoaderResult::ErrorCompressedNCA, "Compressed virtual offsets are not strictly increasing");
                if (entry.compressionType != NCACompressionType::None && entry.compressionType != NCACompressionType::Zeroed &&
                    entry.compressionType != NCACompressionType::Lz4)
                    throw loader_exception(LoaderResult::ErrorCompressedNCA, "Unsupported compressed entry type");
                if (entry.compressionType == NCACompressionType::Lz4 && entry.physicalSize == 0)
                    throw loader_exception(LoaderResult::ErrorCompressedNCA, "LZ4 entry has zero physical size");
                if (entry.compressionType == NCACompressionType::Lz4 &&
                    (entry.physicalOffset > compressionInfo.bucket.tableOffset ||
                     entry.physicalSize > compressionInfo.bucket.tableOffset - entry.physicalOffset))
                    throw loader_exception(LoaderResult::ErrorCompressedNCA, "LZ4 entry is outside the compressed data range");
                previousVirtualOffset = entry.virtualOffset;
                firstEntry = false;
            }
            compressedBuckets.push_back(ConvertCompressedBucketRaw(rawBucket));
        }

        if (totalEntries != tableHeader.entryCount || compressedBuckets.front().entries.front().virtualOffset != 0)
            throw loader_exception(LoaderResult::ErrorCompressedNCA, "Compressed entry count or first offset is invalid");

        if (compressionInfo.bucket.tableOffset == 0 || compressionInfo.bucket.tableOffset > virtualSize)
            throw loader_exception(LoaderResult::ErrorCompressedNCA, "Compressed data range is invalid");

        auto dataBacking{std::make_shared<RegionBacking>(decryptedBacking, 0, compressionInfo.bucket.tableOffset)};
        try {
            return std::make_shared<CompressedStorage>(dataBacking, compressedBlock, std::move(compressedBuckets), compressedBlock.size);
        } catch (const std::exception &e) {
            throw loader_exception(LoaderResult::ErrorCompressedNCA, e.what());
        }
    }

    u8 NCA::GetKeyGeneration() {
        u8 legacyGen{static_cast<u8>(header.cryptoType)};
        u8 gen{static_cast<u8>(header.cryptoType2)};
        gen = std::max<u8>(legacyGen, gen);
        return gen > 0 ? gen - 1 : gen;
    }

    crypto::KeyStore::Key128 NCA::GetTitleKey() {
        u8 keyGeneration{GetKeyGeneration()};

        auto titleKey{keyStore->GetTitleKey(header.rightsId)};
        auto &titleKek{keyStore->titleKek[keyGeneration]};

        if (!titleKey)
            throw loader_exception(LoaderResult::MissingTitleKey);
        if (!titleKek)
            throw loader_exception(LoaderResult::MissingTitleKek);

        crypto::AesCipher cipher(*titleKek, MBEDTLS_CIPHER_AES_128_ECB);
        cipher.Decrypt(*titleKey);
        return *titleKey;
    }

    crypto::KeyStore::Key128 NCA::GetKeyAreaKey(NcaSectionEncryptionType type) {
        auto keyArea{[this, &type](crypto::KeyStore::IndexedKeys128 &keys) {
            u8 keyGeneration{GetKeyGeneration()};

            auto &keyArea{keys[keyGeneration]};

            if (!keyArea)
                throw loader_exception(LoaderResult::MissingKeyArea);

            size_t keyAreaIndex;
            switch (type) {
                case NcaSectionEncryptionType::XTS:
                    keyAreaIndex = 0;
                    break;
                case NcaSectionEncryptionType::CTR:
                case NcaSectionEncryptionType::BKTR:
                    keyAreaIndex = 2;
                    break;
                default:
                    throw exception("Unsupported NcaSectionEncryptionType");
            }

            crypto::KeyStore::Key128 decryptedKeyArea;
            crypto::AesCipher cipher(*keyArea, MBEDTLS_CIPHER_AES_128_ECB);
            cipher.Decrypt(decryptedKeyArea.data(), header.keyArea[keyAreaIndex].data(), decryptedKeyArea.size());
            return decryptedKeyArea;
        }};

        switch (header.keyIndex) {
            case NCAKeyAreaEncryptionKeyType::Application:
                return keyArea(keyStore->areaKeyApplication);
            case NCAKeyAreaEncryptionKeyType::Ocean:
                return keyArea(keyStore->areaKeyOcean);
            case NCAKeyAreaEncryptionKeyType::System:
                return keyArea(keyStore->areaKeySystem);
        }
    }

    void NCA::ValidateNCA(const NCASectionHeader &sectionHeader) {
        // Both Sparse and Compressed sections are now handled properly via CreateSparseBacking/
        // CreateCompressedBacking, which fall back to throwing ErrorSparseNCA/ErrorCompressedNCA
        // themselves if the bucket tree's magic doesn't validate - nothing left to check upfront here
    }
}
