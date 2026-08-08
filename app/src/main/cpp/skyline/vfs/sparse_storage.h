// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "backing.h"
#include "nca.h"

namespace skyline::vfs {
    /**
     * @brief Resolves an NCA section's "Sparse Storage" bucket tree, remapping virtual reads to
     *        either a decrypted region of the underlying physical data, or to a zero-filled region
     *        for blocks that were never physically stored
     * @note Data blocks are decrypted on demand, per read, rather than through a single upfront
     *       CTR-decrypt pass over the whole physical section: a sparse-compacted block's bytes were
     *       originally encrypted as if they sat at their *virtual* offset, not wherever they were
     *       later physically compacted to, so each block needs its own counter derived from its
     *       virtual position - not from the section's physical layout, which is all a single upfront
     *       decrypt pass could ever give it. See ReadImpl for how that's done.
     * @note This reuses the exact same on-disk bucket tree layout and entry format as vfs::BKTR's
     *       relocation table (both are instances of Nintendo's IndirectStorage). The only semantic
     *       difference is what a zero RelocationEntry::fromPatch means: for BKTR it selects the base
     *       RomFs, for Sparse it means "this virtual region was never written, read back as zero"
     * @url https://switchbrew.org/wiki/NCA#Sparse_Storage
     */
    class SparseStorage : public Backing {
      private:
        std::shared_ptr<Backing> rawBacking; //!< The raw, top-level NCA file backing - deliberately NOT pre-decrypted, since each data block needs its own counter (see class note above)
        crypto::KeyStore::Key128 key; //!< The section's content key
        std::array<u8, 0x10> ctr; //!< The section's standard (non-generation-substituted) upper IV, already reversed into AES-CTR's expected byte order
        size_t sectionPhysicalStart; //!< The physical file offset of the section's start - the fixed base every data block's virtual-position counter is computed relative to
        RelocationBlock block; //!< The L1 offset node of the sparse bucket tree
        std::vector<RelocationBucket> buckets; //!< The L2 entry nodes of the sparse bucket tree
        u64 physicalBaseOffset; //!< NCASparseInfo::physicalOffset - the base that every entry's physical offset is relative to, since sparse-compacted data doesn't start at offset 0 of the section

        std::pair<u64, u64> GetEntryIndex(u64 offset);

        RelocationEntry GetEntry(u64 offset);

        RelocationEntry GetNextEntry(u64 offset);

      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override;

      public:
        /**
         * @param rawBacking The raw, top-level NCA file backing (not pre-decrypted)
         * @param key The section's content key, used to decrypt data blocks on demand
         * @param ctr The section's standard upper IV, already reversed into AES-CTR's expected byte order, used to decrypt data blocks on demand
         * @param sectionPhysicalStart The physical file offset of the section's start
         * @param block The parsed L1 offset node of the sparse bucket tree
         * @param buckets The parsed L2 entry nodes of the sparse bucket tree
         * @param virtualSize The full virtual (uncompacted) size of the section - should be `block.size`, this becomes the exposed `Backing::size`
         * @param physicalBaseOffset NCASparseInfo::physicalOffset, added to every entry's physical offset
         */
        SparseStorage(std::shared_ptr<Backing> rawBacking, crypto::KeyStore::Key128 key, std::array<u8, 0x10> ctr, size_t sectionPhysicalStart,
                      RelocationBlock block, std::vector<RelocationBucket> buckets, u64 virtualSize, u64 physicalBaseOffset);
    };
}
