// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "filesystem.h"
#include "nca.h"

namespace skyline::vfs {

    /**
     * @brief Allows reading patched RomFs
     * @url https://switchbrew.org/wiki/NCA#RomFs_Patching
     */
    class BKTR : public Backing {
      private:
        std::shared_ptr<vfs::Backing> baseRomFs;
        std::shared_ptr<vfs::Backing> bktrRomFs;
        RelocationBlock relocation;
        SubsectionBlock subsection;
        std::vector<RelocationBucket> relocationBuckets;
        std::vector<SubsectionBucket> subsectionBuckets;
        bool isEncrypted;
        u64 baseOffset;
        u64 ivfcOffset;
        std::array<u8, 8> sectionCtr;
        std::array<u8, 16> key;

        SubsectionEntry GetNextSubsectionEntry(u64 offset);

        RelocationEntry GetRelocationEntry(u64 offset);

        RelocationEntry GetNextRelocationEntry(u64 offset);

        SubsectionEntry GetSubsectionEntry(u64 offset);

        std::array<u8, 16> GetCipherIV(SubsectionEntry subsectionEntry, u64 sectionOffset);

      public:

        /** @brief Compose already decrypted whole sections; AES-CTR-Ex is resolved by NCA first. */
        BKTR(std::shared_ptr<Backing> original, std::shared_ptr<Backing> patch, RelocationBlock relocation,
             std::vector<RelocationBucket> buckets);

        BKTR(std::shared_ptr<vfs::Backing> pBaseRomfs, std::shared_ptr<vfs::Backing> pBktrRomfs, RelocationBlock pRelocation,
             std::vector<RelocationBucket> pRelocationBuckets, SubsectionBlock pSubsection,
             std::vector<SubsectionBucket> pSubsectionBuckets, bool pIsEncrypted, std::array<u8, 16> pKey,
             u64 pBaseOffset, u64 pIvfcOffset, std::array<u8, 8> pSectionCtr);

        size_t ReadImpl(span<u8> output, size_t offset) override;
        size_t ReadWithPartition(span<u8> output, size_t length, size_t offset);
    };
}
