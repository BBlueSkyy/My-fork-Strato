// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <crypto/key_store.h>
#include <crypto/aes_cipher.h>
#include "filesystem.h"

namespace skyline {
    namespace constant {
        constexpr size_t MediaUnitSize{0x200}; //!< The unit size of entries in an NCA
    }

    namespace vfs {
        enum class NcaContentType : u8 {
            Program = 0x0,
            Meta = 0x1,
            Control = 0x2,
            Manual = 0x3,
            Data = 0x4,
            PublicData = 0x5,
        };

        class NCA {
          private:
            enum class NcaDistributionType : u8 {
                System = 0x0,
                GameCard = 0x1,
            };

            enum class NcaLegacyKeyGenerationType : u8 {
                Fw100 = 0x0,
                Fw300 = 0x2,
            };

            enum class NcaKeyGenerationType : u8 {
                Fw301 = 0x3,
                Fw400 = 0x4,
                Fw500 = 0x5,
                Fw600 = 0x6,
                Fw620 = 0x7,
                Fw700 = 0x8,
                Fw810 = 0x9,
                Fw900 = 0xA,
                Fw910 = 0xB,
                Invalid = 0xFF,
            };

            enum class NcaKeyAreaEncryptionKeyType : u8 {
                Application = 0x0,
                Ocean = 0x1,
                System = 0x2,
            };

            struct NcaFsEntry {
                u32 startOffset;
                u32 endOffset;
                u64 _pad_;
            };

            enum class NcaSectionFsType : u8 {
                RomFs = 0x0,
                PFS0 = 0x1,
            };

            enum class NcaSectionHashType : u8 {
                HierarchicalSha256 = 0x2,
                HierarchicalIntegrity = 0x3,
            };

            enum class NcaSectionEncryptionType : u8 {
                None = 0x1,
                XTS = 0x2,
                CTR = 0x3,
                BKTR = 0x4,
            };

            struct HierarchicalIntegrityLevel {
                u64 offset;
                u64 size;
                u32 blockSize;
                u32 _pad_;
            };
            static_assert(sizeof(HierarchicalIntegrityLevel) == 0x18);

            struct HierarchicalIntegrityHashInfo {
                std::array<u8, 0x20> masterHash;
                u32 magicNumber;
                u32 numLevels;
                std::array<HierarchicalIntegrityLevel, 6> levels;
                u8 _pad0_[0x20];
                std::array<u8, 0x20> masterHash2;
                u8 _pad1_[0x18];
            };
            static_assert(sizeof(HierarchicalIntegrityHashInfo) == 0xF8);

            struct HierarchicalSha256HashInfo {
                std::array<u8, 0x20> hashTableHash;
                u32 blockSize;
                u32 _pad_;
                u64 hashTableOffset;
                u64 hashTableSize;
                u64 pfs0Offset;
                u64 pfs0Size;
                u8 _pad1_[0xB0];
            };
            static_assert(sizeof(HierarchicalSha256HashInfo) == 0xF8);

            struct NcaPatchInfo {
                u64 indirectOffset;
                u64 indirectSize;
                std::array<u8, 0x10> indirectHeader;
                u64 aesCtrExOffset;
                u64 aesCtrExSize;
                std::array<u8, 0x10> aesCtrExHeader;
            };
            static_assert(sizeof(NcaPatchInfo) == 0x40);

            struct NcaSparseInfo {
                u64 tableOffset;
                u64 tableSize;
                std::array<u8, 0x10> tableHeader;
                u64 physicalOffset;
                u16 generation;
                u8 reserved[0x6];
            };
            static_assert(sizeof(NcaSparseInfo) == 0x30);

            struct NcaCompressionInfo {
                u64 tableOffset;
                u64 tableSize;
                std::array<u8, 0x10> tableHeader;
                u64 reserved;
            };
            static_assert(sizeof(NcaCompressionInfo) == 0x28);

            struct NcaMetaDataHashDataInfo {
                u64 tableOffset;
                u64 tableSize;
                std::array<u8, 0x20> tableHash;
            };
            static_assert(sizeof(NcaMetaDataHashDataInfo) == 0x30);

            struct NcaSectionHeader {
                u16 version;
                NcaSectionFsType fsType;
                NcaSectionHashType hashType;
                NcaSectionEncryptionType encryptionType;
                u8 _pad0_[0x3];
                union {
                    HierarchicalIntegrityHashInfo integrityHashInfo;
                    HierarchicalSha256HashInfo sha256HashInfo;
                };

                NcaPatchInfo patchInfo;
                u32 generation;
                u32 secureValue;
                NcaSparseInfo sparseInfo;
                NcaCompressionInfo compressionInfo;
                NcaMetaDataHashDataInfo metaDataHashDataInfo;
                u8 _pad3_[0x30];
            };
            static_assert(sizeof(NcaSectionHeader) == 0x200);

            struct NcaHeader {
                std::array<u8, 0x100> fixed_key_sig;
                std::array<u8, 0x100> npdm_key_sig;
                u32 magic;
                NcaDistributionType distributionType;
                NcaContentType contentType;
                NcaLegacyKeyGenerationType legacyKeyGenerationType;
                NcaKeyAreaEncryptionKeyType keyAreaEncryptionKeyType;
                u64 size;
                u64 programId;
                u32 contentIndex;
                u32 sdkVersion;
                NcaKeyGenerationType keyGenerationType;
                u8 fixedKeyGeneration;
                u8 _pad0_[0xE];
                std::array<u8, 0x10> rightsId;
                std::array<NcaFsEntry, 4> fsEntries;
                std::array<std::array<u8, 0x20>, 4> sectionHashes;
                std::array<std::array<u8, 0x10>, 4> encryptedKeyArea;
                u8 _pad1_[0xC0];
                std::array<NcaSectionHeader, 4> sectionHeaders;
            };
            static_assert(sizeof(NcaHeader) == 0xC00);

            std::shared_ptr<Backing> backing;
            std::shared_ptr<crypto::KeyStore> keyStore;
            bool encrypted{false};
            bool rightsIdEmpty;
            bool useKeyArea;

            void ReadPfs0(const NcaSectionHeader &sectionHeader, const NcaFsEntry &entry);
            void ReadRomFs(const NcaSectionHeader &sectionHeader, const NcaFsEntry &entry);
            std::shared_ptr<Backing> CreateBacking(const NcaSectionHeader &sectionHeader, std::shared_ptr<Backing> rawBacking, size_t offset);
            u8 GetKeyGeneration();
            crypto::KeyStore::Key128 GetTitleKey();
            crypto::KeyStore::Key128 GetKeyAreaKey(NcaSectionEncryptionType type);

          public:
            std::shared_ptr<FileSystem> exeFs;
            std::shared_ptr<FileSystem> logo;
            std::shared_ptr<FileSystem> cnmt;
            std::shared_ptr<Backing> romFs;
            NcaHeader header;
            NcaContentType contentType;

            NCA(std::shared_ptr<vfs::Backing> backing, std::shared_ptr<crypto::KeyStore> keyStore, bool useKeyArea = false);
        };
    }
}
