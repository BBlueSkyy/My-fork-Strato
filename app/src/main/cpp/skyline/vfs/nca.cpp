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
        header = {};
        if (backing->size < sizeof(header) ||
            backing->Read(span<u8>(reinterpret_cast<u8 *>(&header), sizeof(header))) != sizeof(header))
            throw loader_exception(LoaderResult::ParsingError, "Truncated NCA header");

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

        // FS indices are part of the patch contract. Counting present sections loses holes.
        if (backing->size < constant::SectionHeaderOffset + sizeof(sections))
            throw loader_exception(LoaderResult::ParsingError, "Truncated NCA section headers");
        if (backing->Read(span<u8>(reinterpret_cast<u8 *>(sections.data()), sizeof(sections)), constant::SectionHeaderOffset) != sizeof(sections))
            throw loader_exception(LoaderResult::ParsingError, "Short NCA section header read");
        if (encrypted) {
            crypto::AesCipher cipher(*keyStore->headerKey, MBEDTLS_CIPHER_AES_128_XTS);
            cipher.XtsDecrypt({reinterpret_cast<u8 *>(sections.data()), sizeof(sections)}, 2, constant::SectionHeaderSize);
        }

        if (parseMode == NCAParseMode::MetadataOnly && contentType != NCAContentType::Meta && contentType != NCAContentType::Control)
            return;

        for (size_t i{}; i < sections.size(); ++i) {
            if (!HasSection(i))
                continue;
            const auto &section{sections[i]};
            ValidateNCA(section);
            if (section.raw.header.fsType == NcaSectionFsType::RomFs) {
                // Retain the NCA itself as a candidate; a physical patch section is NOT a RomFS.
                if (section.bktr.relocation.size == 0)
                    romFs = BuildRomFsBacking(i);
            } else if (section.raw.header.fsType == NcaSectionFsType::PFS0) {
                auto pfs{OpenPfs0(i)};
                if (contentType == NCAContentType::Program) {
                    if (pfs->FileExists("main") && pfs->FileExists("main.npdm"))
                        exeFs = pfs;
                    else if (pfs->FileExists("NintendoLogo.png") && pfs->FileExists("StartupMovie.gif"))
                        logo = pfs;
                } else if (contentType == NCAContentType::Meta) {
                    cnmt = pfs;
                }
            }
        }
    }

    bool NCA::HasSection(size_t index) const {
        return index < sections.size() && header.sectionTables[index].mediaOffset != 0;
    }

    bool NCA::HasBktrSection() const {
        for (size_t i{}; i < sections.size(); ++i)
            if (HasSection(i) && sections[i].bktr.relocation.size != 0)
                return true;
        return false;
    }

    bool NCA::HasRomFsSection() const {
        for (size_t i{}; i < sections.size(); ++i)
            if (HasSection(i) && sections[i].raw.header.fsType == NcaSectionFsType::RomFs)
                return true;
        return false;
    }

    namespace {
        bool InRange(u64 offset, u64 size, u64 limit) {
            return offset <= limit && size <= limit - offset;
        }

        template<typename T>
        T ReadExact(const std::shared_ptr<Backing> &backing, size_t offset = 0) {
            T value{};
            if (backing->Read(span<u8>(reinterpret_cast<u8 *>(&value), sizeof(value)), offset) != sizeof(value))
                throw loader_exception(LoaderResult::ParsingError, "Short NCA metadata read");
            return value;
        }

        size_t ValidatePatchTable(const BKTRHeader &info, size_t entryStorageSize, size_t backingSize) {
            if (info.magic != util::MakeMagic<u32>("BKTR") || info.version > 1 || info.numberEntries == 0)
                throw loader_exception(LoaderResult::ParsingError, "Invalid NCA patch BucketTree header");
            const size_t nodeSize{QuerySingleLevelNodeStorageSize(entryStorageSize)};
            if (!InRange(info.offset, info.size, backingSize) || !InRange(nodeSize, entryStorageSize, info.size))
                throw loader_exception(LoaderResult::ParsingError, "NCA patch table is outside its backing");
            return nodeSize;
        }

        // Each extent owns a CTR backing with its own generation and absolute counter offset.
        // In particular, the indirect table is read THROUGH this layer, not ordinary AES-CTR.
        class AesCtrExBacking : public Backing {
          public:
            struct Extent {
                u64 start;
                std::shared_ptr<Backing> backing;
            };
            std::vector<Extent> extents;
            explicit AesCtrExBacking(size_t size) : Backing({true, false, false}, size) {}
            size_t ReadImpl(span<u8> output, size_t offset) override {
                if (!InRange(offset, output.size(), size))
                    throw loader_exception(LoaderResult::ParsingError, "AES-CTR-Ex read outside data range");
                size_t done{};
                while (done < output.size()) {
                    const u64 position{offset + done};
                    auto it{std::upper_bound(extents.begin(), extents.end(), position,
                        [](u64 value, const Extent &extent) { return value < extent.start; })};
                    if (it == extents.begin())
                        throw loader_exception(LoaderResult::ParsingError, "Missing AES-CTR-Ex extent");
                    --it;
                    const size_t local{static_cast<size_t>(position - it->start)};
                    const size_t count{std::min(output.size() - done, it->backing->size - local)};
                    if (count == 0 || it->backing->Read(output.subspan(done, count), local) != count)
                        throw loader_exception(LoaderResult::ParsingError, "Short AES-CTR-Ex read");
                    done += count;
                }
                return done;
            }
        };
    }

    std::shared_ptr<Backing> NCA::CreateAesCtrExBacking(const NCASectionHeader &section, std::shared_ptr<Backing> raw, size_t offset) {
        const auto &info{section.bktr.subsection};
        const auto &indirect{section.bktr.relocation};
        const auto encryption{section.raw.header.encryptionType};
        if (encryption != NcaSectionEncryptionType::BKTR && encryption != NcaSectionEncryptionType::None)
            throw loader_exception(LoaderResult::ParsingError, "AES-CTR-Ex table with incompatible encryption type");
        if (indirect.size == 0 || !InRange(indirect.offset, indirect.size, info.offset))
            throw loader_exception(LoaderResult::ParsingError, "Indirect table overlaps AES-CTR-Ex metadata");
        const size_t entrySize{QuerySubsectionEntryStorageSize(info.numberEntries)};
        const size_t nodeSize{ValidatePatchTable(info, entrySize, raw->size)};
        auto metadata{CreateBacking(section, raw, offset)};
        auto root{ReadExact<SubsectionBlock>(metadata, info.offset)};
        ValidateRootBlock(root, entrySize / BucketNodeSize, "AES-CTR-Ex");
        if (root.size != info.offset)
            throw loader_exception(LoaderResult::ParsingError, "AES-CTR-Ex data size does not match its table offset");

        std::vector<SubsectionEntry> entries;
        entries.reserve(info.numberEntries);
        for (size_t i{}; i < root.numberBuckets; ++i) {
            const auto bucket{ReadExact<SubsectionBucketRaw>(metadata, info.offset + nodeSize + i * BucketNodeSize)};
            const u64 end{i + 1 < root.numberBuckets ? root.baseOffsets[i + 1] : root.size};
            if (bucket.index != i || bucket.numberEntries == 0 || bucket.numberEntries > bucket.subsectionEntries.size() ||
                bucket.endOffset != end || bucket.subsectionEntries[0].addressPatch != root.baseOffsets[i])
                throw loader_exception(LoaderResult::ParsingError, "Invalid AES-CTR-Ex entry bucket");
            for (size_t j{}; j < bucket.numberEntries; ++j) {
                const auto &entry{bucket.subsectionEntries[j]};
                if (entry.addressPatch >= end || (entry.addressPatch & 0xF) || entry._pad0_[0] > 1 ||
                    (!entries.empty() && entries.back().addressPatch >= entry.addressPatch))
                    throw loader_exception(LoaderResult::ParsingError, "Invalid AES-CTR-Ex entry");
                entries.push_back(entry);
            }
        }
        if (entries.size() != info.numberEntries)
            throw loader_exception(LoaderResult::ParsingError, "AES-CTR-Ex entry count mismatch");
        auto result{std::make_shared<AesCtrExBacking>(root.size)};
        for (size_t i{}; i < entries.size(); ++i) {
            const auto &entry{entries[i]};
            const u64 end{i + 1 < entries.size() ? entries[i + 1].addressPatch : root.size};
            std::shared_ptr<Backing> extent{std::make_shared<RegionBacking>(raw, entry.addressPatch, end - entry.addressPatch)};
            if (encrypted && encryption != NcaSectionEncryptionType::None && entry._pad0_[0] == 0) {
                auto ctrSection{section};
                std::memcpy(ctrSection.raw.sectionCtr.data(), &entry.ctr, sizeof(entry.ctr));
                extent = CreateBacking(ctrSection, extent, offset + entry.addressPatch);
            }
            result->extents.push_back({entry.addressPatch, std::move(extent)});
        }
        return result;
    }

    std::shared_ptr<Backing> NCA::OpenRawSection(size_t index) {
        if (!HasSection(index))
            throw loader_exception(LoaderResult::ParsingError, "Missing NCA section");
        if (rawSections[index])
            return rawSections[index];
        const auto &entry{header.sectionTables[index]};
        const auto &section{sections[index]};
        const u64 start{static_cast<u64>(entry.mediaOffset) * constant::MediaUnitSize};
        const u64 end{static_cast<u64>(entry.mediaEndOffset) * constant::MediaUnitSize};
        if (start < constant::SectionHeaderOffset + sizeof(sections) || end <= start ||
            (section.raw.sparseInfo.generation == 0 && end > backing->size))
            throw loader_exception(LoaderResult::ParsingError, "Invalid NCA section extent");
        std::shared_ptr<Backing> raw{std::make_shared<RegionBacking>(backing, start, end - start)};
        raw = CreateSparseBacking(section, raw);
        if (section.bktr.subsection.size != 0)
            raw = CreateAesCtrExBacking(section, raw, start);
        else {
            if (section.raw.header.encryptionType == NcaSectionEncryptionType::BKTR)
                throw loader_exception(LoaderResult::ParsingError, "AES-CTR-Ex section is missing its table");
            raw = CreateBacking(section, raw, start);
        }
        if (!raw)
            throw loader_exception(LoaderResult::ParsingError, "Unsupported NCA encryption type");
        rawSections[index] = raw;
        return raw;
    }

    std::shared_ptr<FileSystem> NCA::OpenPfs0(size_t index) {
        if (partitionSections[index])
            return partitionSections[index];
        const auto &section{sections[index]};
        if (section.raw.header.hashType != NcaSectionHashType::HierarchicalSha256 || section.bktr.relocation.size != 0)
            throw loader_exception(LoaderResult::ParsingError, "Unsupported Program partition hash/patch type");
        const u32 count{section.pfs0.layerCount};
        if (count == 0 || count > 5)
            throw loader_exception(LoaderResult::ParsingError, "Invalid PFS0 hash level count");
        std::array<u64, 2> dataLevel{};
        std::memcpy(dataLevel.data(), section.raw.blockData.data() + 0x28 + (count - 1) * 0x10, sizeof(dataLevel));
        auto raw{OpenRawSection(index)};
        if (dataLevel[1] == 0 || !InRange(dataLevel[0], dataLevel[1], raw->size))
            throw loader_exception(LoaderResult::ParsingError, "PFS0 data level is outside the section");
        auto data{CreateCompressedBacking(section, std::make_shared<RegionBacking>(raw, dataLevel[0], dataLevel[1]), dataLevel[1])};
        // Validate names and file extents before PartitionFileSystem indexes the name table.
        const auto pfsHeader{ReadExact<std::array<u32, 4>>(data)};
        const u64 namesOffset{0x10 + static_cast<u64>(pfsHeader[1]) * 0x18};
        if (pfsHeader[0] != util::MakeMagic<u32>("PFS0") || !InRange(namesOffset, pfsHeader[2], data->size))
            throw loader_exception(LoaderResult::ParsingError, "Invalid NCA PFS0 table extent");
        const u64 filesOffset{namesOffset + pfsHeader[2]};
        std::vector<u8> names(pfsHeader[2]);
        if (data->Read(names, namesOffset) != names.size())
            throw loader_exception(LoaderResult::ParsingError, "Short PFS0 name table read");
        for (u32 i{}; i < pfsHeader[1]; ++i) {
            const auto extent{ReadExact<std::array<u64, 2>>(data, 0x10 + static_cast<u64>(i) * 0x18)};
            const auto name{ReadExact<u32>(data, 0x20 + static_cast<u64>(i) * 0x18)};
            if (!InRange(extent[0], extent[1], data->size - filesOffset) || name >= names.size() ||
                std::find(names.begin() + name, names.end(), 0) == names.end())
                throw loader_exception(LoaderResult::ParsingError, "Invalid PFS0 file/name extent");
        }
        auto pfs{std::make_shared<PartitionFileSystem>(data)};
        partitionSections[index] = pfs;
        return pfs;
    }

    std::shared_ptr<Backing> NCA::OpenRawStorageWithPatch(NCA &base, size_t index) {
        auto patch{OpenRawSection(index)};
        const auto &info{sections[index].bktr.relocation};
        if (info.size == 0)
            return patch;
        const size_t entrySize{QuerySparseEntryStorageSize(info.numberEntries)};
        const size_t nodeSize{ValidatePatchTable(info, entrySize, patch->size)};
        auto root{ReadExact<RelocationBlock>(patch, info.offset)};
        ValidateRootBlock(root, entrySize / BucketNodeSize, "BKTR indirect");
        // Original offsets address the WHOLE corresponding decrypted section, including hash levels.
        std::shared_ptr<Backing> original{std::make_shared<RegionBacking>(backing, 0, 0)};
        if (base.HasSection(index)) {
            if (base.sections[index].raw.header.fsType != sections[index].raw.header.fsType || base.sections[index].bktr.relocation.size != 0)
                throw loader_exception(LoaderResult::ParsingError, "Incompatible base NCA section for indirect storage");
            original = base.OpenRawSection(index);
        }
        std::vector<RelocationBucket> buckets;
        size_t entries{};
        for (size_t i{}; i < root.numberBuckets; ++i) {
            const auto bucket{ReadExact<RelocationBucketRaw>(patch, info.offset + nodeSize + i * BucketNodeSize)};
            const u64 end{i + 1 < root.numberBuckets ? root.baseOffsets[i + 1] : root.size};
            if (bucket.index != i || bucket.numberEntries == 0 || bucket.numberEntries > bucket.relocationEntries.size() ||
                bucket.endOffset != end || bucket.relocationEntries[0].addressPatch != root.baseOffsets[i])
                throw loader_exception(LoaderResult::ParsingError, "Invalid BKTR relocation bucket");
            for (size_t j{}; j < bucket.numberEntries; ++j) {
                const auto &entry{bucket.relocationEntries[j]};
                const u64 next{j + 1 < bucket.numberEntries ? bucket.relocationEntries[j + 1].addressPatch : end};
                if (entry.fromPatch > 1 || entry.addressPatch >= next ||
                    !InRange(entry.addressSource, next - entry.addressPatch, entry.fromPatch ? info.offset : original->size))
                    throw loader_exception(LoaderResult::ParsingError, "BKTR relocation is outside its physical source");
            }
            entries += bucket.numberEntries;
            buckets.push_back(ConvertRelocationBucketRaw(bucket));
        }
        if (entries != info.numberEntries)
            throw loader_exception(LoaderResult::ParsingError, "BKTR relocation entry count mismatch");
        return std::make_shared<BKTR>(original, std::make_shared<RegionBacking>(patch, 0, info.offset), root, std::move(buckets));
    }

    std::shared_ptr<Backing> NCA::BuildRomFsBacking(size_t index, NCA *base) {
        const auto &section{sections[index]};
        const auto &ivfc{section.romfs.ivfc};
        if (section.raw.header.hashType != NcaSectionHashType::HierarchicalIntegrity ||
            ivfc.magic != util::MakeMagic<u32>("IVFC") || ivfc.levelCount < 2 || ivfc.levelCount > constant::IvfcMaxLevel + 1)
            throw loader_exception(LoaderResult::ParsingError, "Invalid IVFC header/level count (NCA fields must be little endian)");
        auto raw{base ? OpenRawStorageWithPatch(*base, index) : OpenRawSection(index)};
        for (size_t i{}; i < ivfc.levelCount - 1; ++i) {
            const auto &level{ivfc.levels[i]};
            if (level.size == 0 || level.blockSize > 32 || !InRange(level.offset, level.size, raw->size))
                throw loader_exception(LoaderResult::ParsingError, fmt::format("IVFC level {} is outside resolved section (size=0x{:X})", i, raw->size));
        }
        const auto &dataLevel{ivfc.levels[ivfc.levelCount - 2]};
        ivfcOffset = dataLevel.offset;
        rawRomFs = std::make_shared<RegionBacking>(raw, dataLevel.offset, dataLevel.size);
        auto result{CreateCompressedBacking(section, rawRomFs, rawRomFs->size)};
        const auto romHeader{ReadExact<RomFileSystem::RomFsHeader>(result)};
        if (romHeader.headerSize != sizeof(romHeader) || romHeader.dataOffset < sizeof(romHeader) || romHeader.dataOffset > result->size)
            throw loader_exception(LoaderResult::ParsingError, "Invalid resolved RomFS header");
        for (const auto &[offset, size] : {std::pair{romHeader.dirHashTableOffset, romHeader.dirHashTableSize},
             std::pair{romHeader.dirMetaTableOffset, romHeader.dirMetaTableSize},
             std::pair{romHeader.fileHashTableOffset, romHeader.fileHashTableSize},
             std::pair{romHeader.fileMetaTableOffset, romHeader.fileMetaTableSize}})
            if (!InRange(offset, size, result->size) || (size != 0 && offset < sizeof(romHeader)))
                throw loader_exception(LoaderResult::ParsingError, "Resolved RomFS metadata is outside data backing");
        LOGI("RomFS section {} resolved: IVFC data level {}, size=0x{:X}, indirectSize=0x{:X}, aesCtrExSize=0x{:X}",
             index, ivfc.levelCount - 2, result->size, section.bktr.relocation.size, section.bktr.subsection.size);
        return result;
    }

    std::shared_ptr<FileSystem> NCA::OpenExeFsWithPatch(NCA &base) {
        if (contentType != NCAContentType::Program || header.titleId != base.header.titleId)
            throw loader_exception(LoaderResult::ParsingError, "Program patch does not match base Program");
        for (size_t i{}; i < sections.size(); ++i) {
            if (!HasSection(i) || sections[i].raw.header.fsType != NcaSectionFsType::PFS0)
                continue;
            auto pfs{OpenPfs0(i)};
            if (pfs->FileExists("main") && pfs->FileExists("main.npdm")) {
                LOGI("Program patch ExeFS selected (section {})", i);
                return pfs;
            }
        }
        LOGI("Program patch has no executable partition; using base ExeFS");
        return base.exeFs;
    }

    std::shared_ptr<Backing> NCA::OpenRomFsWithPatch(NCA &base) {
        if (contentType != base.contentType || header.titleId != base.header.titleId)
            throw loader_exception(LoaderResult::ParsingError, "RomFS patch does not match base content");
        for (size_t i{}; i < sections.size(); ++i) {
            if (!HasSection(i) || sections[i].raw.header.fsType != NcaSectionFsType::RomFs)
                continue;
            auto result{BuildRomFsBacking(i, &base)};
            if (sections[i].bktr.relocation.size != 0)
                LOGI("BKTR Program patch RomFS constructed (section {})", i);
            romFs = result;
            return result;
        }
        return base.romFs;
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
        throw loader_exception(LoaderResult::ParsingError, "Invalid NCA key-area index");
    }

    void NCA::ValidateNCA(const NCASectionHeader &sectionHeader) {
        const auto &fs{sectionHeader.raw.header};
        if ((fs.fsType != NcaSectionFsType::RomFs && fs.fsType != NcaSectionFsType::PFS0) ||
            (fs.fsType == NcaSectionFsType::RomFs && fs.hashType != NcaSectionHashType::HierarchicalIntegrity) ||
            (fs.fsType == NcaSectionFsType::PFS0 && fs.hashType != NcaSectionHashType::HierarchicalSha256))
            throw loader_exception(LoaderResult::ParsingError, "Unsupported NCA filesystem/hash type pair");
        // Sparse/Compressed validation remains in their existing builders.
    }
}
