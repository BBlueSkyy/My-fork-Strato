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

        // romFsSize is the *virtual* (fully decompressed) RomFs size taken from the IVFC header - it's
        // only correct as the raw on-disk window's length when the section is stored 1:1 (no Sparse/
        // Compressed Storage). For a Sparse or Compressed section, the physical footprint on disk is
        // smaller than romFsSize, so we derive the real physical window from the section table entry
        // instead (same calculation ReadPfs0 already uses for its own section), and reserve romFsSize
        // for the virtualSize passed to CreateSparseBacking/CreateCompressedBacking below, which is what
        // those wrappers should expose to RomFs parsing code afterwards.
        const std::size_t physicalSectionSize{constant::MediaUnitSize * static_cast<size_t>(entry.mediaEndOffset - entry.mediaOffset)};
        const std::size_t physicalRomFsSize{physicalSectionSize > ivfcOffset ? physicalSectionSize - ivfcOffset : 0};

        auto decryptedBacking{CreateBacking(sectionHeader, std::make_shared<RegionBacking>(backing, romFsOffset, physicalRomFsSize), romFsOffset)};
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

    struct BucketTreeHeader {
        u32 magic;
        u32 version;
        u32 entryCount;
        u32 _pad_;
    };
    static_assert(sizeof(BucketTreeHeader) == 0x10);

    auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.raw.header.encryptionType)};

    // The table (node+entry storage) uses a *different* upper IV whenever sparseInfo.generation != 0:
    // Generation gets replaced by (generation << 16), SecureValue stays the same - confirmed against
    // NcaSparseInfo.MakeAesCtrUpperIv/NcaAesCtrUpperIv in LibHac. No-op when generation == 0.
    std::array<u8, 0x10> tableCtr{};
    for (std::size_t i{}; i < 4; ++i)
        tableCtr[i] = sectionHeader.raw.sectionCtr[7 - i]; // SecureValue, unchanged, big-endian

    const u32 sparseGen{static_cast<u32>(sparseInfo.generation) << 16};
    tableCtr[4] = static_cast<u8>((sparseGen >> 24) & 0xFF);
    tableCtr[5] = static_cast<u8>((sparseGen >> 16) & 0xFF);
    tableCtr[6] = 0;
    tableCtr[7] = 0;

    // The table sits at a fixed physical position and isn't remapped - its counter is just that physical
    // position (sectionPhysicalStart + local offset), with the substituted Generation above.
    auto tableRegion{std::make_shared<RegionBacking>(backing, sectionPhysicalStart, sparseInfo.bucket.tableOffset + sparseInfo.bucket.tableSize)};
    CtrEncryptedBacking tableBacking{tableCtr, key, tableRegion, sectionPhysicalStart};

    BucketTreeHeader treeHeader{};
    tableBacking.Read(span(reinterpret_cast<u8 *>(&treeHeader), sizeof(treeHeader)), sparseInfo.bucket.tableOffset);
    if (treeHeader.magic != util::MakeMagic<u32>("BKTR"))
        throw loader_exception(LoaderResult::ErrorSparseNCA);

    const size_t nodeStorageOffset{sparseInfo.bucket.tableOffset + sizeof(BucketTreeHeader)};
    RelocationBlock sparseBlock{};
    tableBacking.Read(span(reinterpret_cast<u8 *>(&sparseBlock), sizeof(sparseBlock)), nodeStorageOffset);

    const size_t entryStorageOffset{nodeStorageOffset + sizeof(RelocationBlock)};
    const size_t entryStorageSize{sparseInfo.bucket.tableSize - sizeof(BucketTreeHeader) - sizeof(RelocationBlock)};

    std::vector<RelocationBucketRaw> sparseBucketsRaw(entryStorageSize / sizeof(RelocationBucketRaw));
    for (std::size_t i{}; i < sparseBucketsRaw.size(); ++i)
        tableBacking.Read(span(reinterpret_cast<u8 *>(&sparseBucketsRaw[i]), sizeof(RelocationBucketRaw)), entryStorageOffset + i * sizeof(RelocationBucketRaw));

    std::vector<RelocationBucket> sparseBuckets;
    sparseBuckets.reserve(sparseBucketsRaw.size());
    for (const auto &rawBucket : sparseBucketsRaw)
        sparseBuckets.push_back(ConvertRelocationBucketRaw(rawBucket));

    // Standard ctr for the actual data blocks - normal Generation field, unlike the table above
    std::array<u8, 0x10> dataCtr{};
    for (std::size_t i{}; i < 8; ++i)
        dataCtr[i] = sectionHeader.raw.sectionCtr[8 - i - 1];

        return std::make_shared<SparseStorage>(backing, key, dataCtr, sectionPhysicalStart, sparseBlock, std::move(sparseBuckets), sparseBlock.size, sparseInfo.physicalOffset);
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
