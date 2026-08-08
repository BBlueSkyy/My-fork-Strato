// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <crypto/aes_cipher.h>
#include <loader/loader.h>

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

    NCA::NCA(std::shared_ptr<vfs::Backing> pBacking, std::shared_ptr<crypto::KeyStore> pKeyStore, bool pUseKeyArea)
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
            for (size_t i{}; i < lengthSections; i++)
                sections.push_back(backing->Read<NCASectionHeader>());
        }

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
        : romFs(updateNca->romFs), header(updateNca->header), sections(std::move(updateNca->sections)), encrypted(updateNca->encrypted), backing(std::move(updateNca->backing)),
        keyStore(std::move(pKeyStore)), bktrBaseRomfs(std::move(bktrBaseRomfs)), bktrBaseIvfcOffset(bktrBaseIvfcOffset), useKeyArea(pUseKeyArea) {

        useKeyArea = false;
        contentType = header.contentType;
        rightsIdEmpty = header.rightsId == crypto::KeyStore::Key128{};

        if (!updateNca)
            throw loader_exception(LoaderResult::ParsingError);

        for (std::size_t i = 0; i < sections.size(); ++i) {
            const auto &section = sections[i];

            ValidateNCA(section);

            if (section.raw.header.fsType == NcaSectionFsType::RomFs)
                ReadRomFs(section, header.sectionTables[i]);
        }
    }

    void NCA::ReadPfs0(const NCASectionHeader &section, const NCASectionTableEntry &entry) {
        size_t offset{static_cast<size_t>(entry.mediaOffset) * constant::MediaUnitSize + section.pfs0.pfs0HeaderOffset};
        size_t size{constant::MediaUnitSize * static_cast<size_t>(entry.mediaEndOffset - entry.mediaOffset)};

        auto sectionBacking{CreateBacking(section, std::make_shared<RegionBacking>(backing, offset, size), offset)};
        sectionBacking = CreateSparseBacking(section, sectionBacking, offset);
        sectionBacking = CreateCompressedBacking(section, sectionBacking, size);
        auto pfs{std::make_shared<PartitionFileSystem>(sectionBacking)};

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
        const std::size_t baseOffset{entry.mediaOffset * constant::MediaUnitSize};
        ivfcOffset = sectionHeader.romfs.ivfc.levels[constant::IvfcMaxLevel - 1].offset;
        const std::size_t romFsOffset{baseOffset + ivfcOffset};
        const std::size_t romFsSize{sectionHeader.romfs.ivfc.levels[constant::IvfcMaxLevel - 1].size};
        auto decryptedBacking{CreateBacking(sectionHeader, std::make_shared<RegionBacking>(backing, romFsOffset, romFsSize), romFsOffset)};
        decryptedBacking = CreateSparseBacking(sectionHeader, decryptedBacking, romFsOffset);
        decryptedBacking = CreateCompressedBacking(sectionHeader, decryptedBacking, romFsSize);

        if (sectionHeader.raw.header.encryptionType == NcaSectionEncryptionType::BKTR && bktrBaseRomfs && romFs) {
            const u64 size{constant::MediaUnitSize * (entry.mediaEndOffset - entry.mediaOffset)};
            const u64 offset{sectionHeader.romfs.ivfc.levels[constant::IvfcMaxLevel - 1].offset};

            RelocationBlock relocationBlock{romFs->Read<RelocationBlock>(sectionHeader.bktr.relocation.offset - offset)};
            SubsectionBlock subsectionBlock{romFs->Read<SubsectionBlock>(sectionHeader.bktr.subsection.offset - offset)};

            std::vector<RelocationBucketRaw> relocationBucketsRaw((sectionHeader.bktr.relocation.size - sizeof(RelocationBlock)) / sizeof(RelocationBucketRaw));
            auto regionBackingRelocation{std::make_shared<RegionBacking>(romFs, sectionHeader.bktr.relocation.offset + sizeof(RelocationBlock) - offset, sectionHeader.bktr.relocation.size - sizeof(RelocationBlock))};
            regionBackingRelocation->Read<RelocationBucketRaw>(relocationBucketsRaw);

            std::vector<SubsectionBucketRaw> subsectionBucketsRaw((sectionHeader.bktr.subsection.size - sizeof(SubsectionBlock)) / sizeof(SubsectionBucketRaw));
            auto regionBackingSubsection{std::make_shared<RegionBacking>(romFs, sectionHeader.bktr.subsection.offset + sizeof(SubsectionBlock) - offset, sectionHeader.bktr.subsection.size - sizeof(SubsectionBlock))};
            regionBackingSubsection->Read<SubsectionBucketRaw>(subsectionBucketsRaw);

            std::vector<RelocationBucket> relocationBuckets;
            relocationBuckets.reserve(relocationBucketsRaw.size());
            for (const RelocationBucketRaw &rawBucket : relocationBucketsRaw)
                relocationBuckets.push_back(ConvertRelocationBucketRaw(rawBucket));

            std::vector<SubsectionBucket> subsectionBuckets;
            subsectionBuckets.reserve(subsectionBucketsRaw.size());
            for (const SubsectionBucketRaw &rawBucket : subsectionBucketsRaw)
                subsectionBuckets.push_back(ConvertSubsectionBucketRaw(rawBucket));

            u32 ctrLow;
            std::memcpy(&ctrLow, sectionHeader.raw.sectionCtr.data(), sizeof(ctrLow));
            subsectionBuckets.back().entries.push_back({sectionHeader.bktr.relocation.offset, {0}, ctrLow});
            subsectionBuckets.back().entries.push_back({size, {0}, 0});

            auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.raw.header.encryptionType)};

            auto bktr{std::make_shared<BKTR>(
                bktrBaseRomfs, std::make_shared<RegionBacking>(backing, baseOffset, romFsSize),
                relocationBlock, relocationBuckets, subsectionBlock, subsectionBuckets, encrypted,
                encrypted ? key : std::array<u8, 0x10>{}, baseOffset, bktrBaseIvfcOffset,
                sectionHeader.raw.sectionCtr)};

            romFs = std::make_shared<RegionBacking>(bktr, sectionHeader.romfs.ivfc.levels[constant::IvfcMaxLevel - 1].offset, romFsSize);
        } else {
            romFs = std::move(decryptedBacking);
        }
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

    std::shared_ptr<Backing> NCA::CreateSparseBacking(const NCASectionHeader &sectionHeader, std::shared_ptr<Backing> decryptedBacking, size_t sectionPhysicalStart) {
        const auto &sparseInfo{sectionHeader.raw.sparseInfo};
        if (sparseInfo.bucket.tableOffset == 0 || sparseInfo.bucket.tableSize == 0)
            return decryptedBacking;

        // The BucketTree header (magic "BKTR", version, entryCount, reserved - 0x10 bytes total) is NOT
        // a separate record stored at tableOffset in the backing. It's embedded directly in the FS
        // header itself, as NCABucketInfo::tableHeader - already decrypted along with the rest of the
        // section header, no extra read needed. tableOffset points straight at the node storage
        // (RelocationBlock/L1) - there's no header to skip over first. Confirmed against the fork's own
        // existing BKTR/PatchInfo handling in ReadRomFs, which reads RelocationBlock directly at
        // sectionHeader.bktr.relocation.offset with no header read beforehand either.
        u32 magic;
        std::memcpy(&magic, sparseInfo.bucket.tableHeader.data(), sizeof(magic));
        if (magic != util::MakeMagic<u32>("BKTR"))
            throw loader_exception(LoaderResult::ErrorSparseNCA, fmt::format("magic=0x{:X} tableOffset=0x{:X} tableSize=0x{:X}", magic, sparseInfo.bucket.tableOffset, sparseInfo.bucket.tableSize));

        auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.raw.header.encryptionType)};

        // The table (node + entry storage) is decrypted with a *different* upper IV whenever
        // sparseInfo.generation != 0: the Generation field gets replaced with (generation << 16),
        // SecureValue is untouched - confirmed against NcaSparseInfo.MakeAesCtrUpperIv/NcaAesCtrUpperIv
        // in LibHac (Generation = low 4 bytes of the 8-byte upper IV, SecureValue = high 4 bytes; our
        // ctr[] arrays hold both reversed into big-endian, so Generation lands in ctr[4..7]). This is a
        // no-op when generation == 0 (the common case for base-game NCAs, as opposed to updates/DLC).
        std::array<u8, 0x10> tableCtr{};
        for (std::size_t i{}; i < 4; ++i)
            tableCtr[i] = sectionHeader.raw.sectionCtr[7 - i]; // SecureValue, unchanged, big-endian

        const u32 sparseGen{static_cast<u32>(sparseInfo.generation) << 16};
        tableCtr[4] = static_cast<u8>((sparseGen >> 24) & 0xFF);
        tableCtr[5] = static_cast<u8>((sparseGen >> 16) & 0xFF);
        tableCtr[6] = 0;
        tableCtr[7] = 0;

        // The table itself sits at a fixed physical position and isn't remapped - its counter is just
        // that physical position (sectionPhysicalStart + local offset), with the substituted Generation
        // above. This is unlike the actual sparse *data* blocks below, which need a virtual-position
        // counter instead (see SparseStorage::ReadImpl for why).
        auto tableRegion{std::make_shared<RegionBacking>(backing, sectionPhysicalStart, sparseInfo.bucket.tableOffset + sparseInfo.bucket.tableSize)};
        CtrEncryptedBacking tableBacking{tableCtr, key, tableRegion, sectionPhysicalStart};

        RelocationBlock sparseBlock{tableBacking.Read<RelocationBlock>(sparseInfo.bucket.tableOffset)};

        const size_t entryStorageOffset{sparseInfo.bucket.tableOffset + sizeof(RelocationBlock)};
        const size_t entryStorageSize{sparseInfo.bucket.tableSize - sizeof(RelocationBlock)};

        std::vector<RelocationBucketRaw> sparseBucketsRaw(entryStorageSize / sizeof(RelocationBucketRaw));
        for (std::size_t i{}; i < sparseBucketsRaw.size(); ++i)
            sparseBucketsRaw[i] = tableBacking.Read<RelocationBucketRaw>(entryStorageOffset + i * sizeof(RelocationBucketRaw));

        std::vector<RelocationBucket> sparseBuckets;
        sparseBuckets.reserve(sparseBucketsRaw.size());
        for (const auto &rawBucket : sparseBucketsRaw)
            sparseBuckets.push_back(ConvertRelocationBucketRaw(rawBucket));

        // Standard ctr (normal Generation field, unlike the table above) for the actual data blocks -
        // SparseStorage decrypts these on demand rather than through decryptedBacking, since it needs a
        // per-block counter based on virtual position, not the section's physical layout.
        std::array<u8, 0x10> dataCtr{};
        for (std::size_t i{}; i < 8; ++i)
            dataCtr[i] = sectionHeader.raw.sectionCtr[8 - i - 1];

        return std::make_shared<SparseStorage>(backing, key, dataCtr, sectionPhysicalStart, sparseBlock, std::move(sparseBuckets), sparseBlock.size, sparseInfo.physicalOffset);
    }

    std::shared_ptr<Backing> NCA::CreateCompressedBacking(const NCASectionHeader &sectionHeader, std::shared_ptr<Backing> decryptedBacking, size_t virtualSize) {
        const auto &compressionInfo{sectionHeader.raw.compressionInfo};
        if (compressionInfo.bucket.tableOffset == 0 || compressionInfo.bucket.tableSize == 0)
            return decryptedBacking;

        // Same fix as CreateSparseBacking above: the BucketTreeHeader is compressionInfo.bucket.tableHeader
        // itself (already decrypted with the rest of the FS header) - not a separate record stored at
        // tableOffset. tableOffset points straight at the node storage.
        u32 magic;
        std::memcpy(&magic, compressionInfo.bucket.tableHeader.data(), sizeof(magic));
        if (magic != util::MakeMagic<u32>("BKTR"))
            throw loader_exception(LoaderResult::ErrorCompressedNCA, fmt::format("magic=0x{:X} tableOffset=0x{:X} tableSize=0x{:X}", magic, compressionInfo.bucket.tableOffset, compressionInfo.bucket.tableSize));

        RelocationBlock compressedBlock{decryptedBacking->Read<RelocationBlock>(compressionInfo.bucket.tableOffset)};

        const size_t entryStorageOffset{compressionInfo.bucket.tableOffset + sizeof(RelocationBlock)};
        const size_t entryStorageSize{compressionInfo.bucket.tableSize - sizeof(RelocationBlock)};

        std::vector<CompressedBucketRaw> compressedBucketsRaw(entryStorageSize / sizeof(CompressedBucketRaw));
        auto regionBackingCompressed{std::make_shared<RegionBacking>(decryptedBacking, entryStorageOffset, entryStorageSize)};
        regionBackingCompressed->Read<CompressedBucketRaw>(compressedBucketsRaw);

        std::vector<CompressedBucket> compressedBuckets;
        compressedBuckets.reserve(compressedBucketsRaw.size());
        for (const auto &rawBucket : compressedBucketsRaw)
            compressedBuckets.push_back(ConvertCompressedBucketRaw(rawBucket));

        // Use the bucket tree's own declared virtual (decompressed) size here, *not* the physical
        // on-disk `virtualSize` parameter above (that's the section's physical span from the NCA's
        // section table - always >= the true virtual size for a compressed section, since compression
        // only shrinks). Passing the physical size as the exposed Backing::size made reads past the
        // compressed footprint throw "past the end of a backing", even though they were valid within
        // the real decompressed range.
        return std::make_shared<CompressedStorage>(decryptedBacking, compressedBlock, std::move(compressedBuckets), compressedBlock.size);
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
        // themselves if the bucket tree's magic (read from the embedded tableHeader) doesn't validate -
        // nothing left to check upfront here
    }
}
