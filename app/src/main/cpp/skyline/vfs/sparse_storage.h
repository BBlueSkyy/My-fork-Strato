// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "backing.h"
#include "nca.h"

namespace skyline::vfs {
    /**
     * @brief Resolves an NCA section's sparse bucket tree, remapping logical reads to encrypted
     *        physical data or to zero-filled regions
     * @note This layer deliberately does not decrypt data. AES-CTR/AES-CTR-Ex must wrap the sparse
     *       logical view afterwards so counters are derived from logical rather than compacted offsets.
     * @note This reuses the exact same on-disk bucket tree layout and entry format as vfs::BKTR's
     *       relocation table (both are instances of Nintendo's IndirectStorage). The only semantic
     *       difference is what a zero RelocationEntry::fromPatch means: for BKTR it selects the base
     *       RomFs, for Sparse it means "this virtual region was never written, read back as zero"
     * @url https://switchbrew.org/wiki/NCA#Sparse_Storage
     */
    class SparseStorage : public Backing {
      private:
        std::shared_ptr<Backing> rawBacking; //!< The raw, top-level NCA file backing
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
         * @brief Creates an empty sparse layer. A sparse table with zero entries represents an
         *        all-zero logical section.
         */
        explicit SparseStorage(u64 virtualSize);

        /**
         * @param rawBacking The raw, top-level NCA file backing (not pre-decrypted)
         * @param block The parsed L1 offset node of the sparse bucket tree
         * @param buckets The parsed L2 entry nodes of the sparse bucket tree
         * @param virtualSize The full virtual (uncompacted) size of the section - should be `block.size`, this becomes the exposed `Backing::size`
         * @param physicalBaseOffset NCASparseInfo::physicalOffset, added to every entry's physical offset
         */
        SparseStorage(std::shared_ptr<Backing> rawBacking, RelocationBlock block, std::vector<RelocationBucket> buckets,
                      u64 virtualSize, u64 physicalBaseOffset);
    };
}
