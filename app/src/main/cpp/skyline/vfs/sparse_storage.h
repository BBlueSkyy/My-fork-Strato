// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "backing.h"
#include "nca.h"
#include "bktr.h"

namespace skyline::vfs {
    /**
     * @brief Resolves an NCA section's "Sparse Storage" bucket tree, remapping virtual reads to
     *        either a region of the underlying (already CTR-decrypted) physical backing, or to a
     *        zero-filled region for blocks that were never physically stored
     * @note This reuses the exact same on-disk bucket tree layout and entry format as vfs::BKTR's
     *       relocation table (both are instances of Nintendo's IndirectStorage). The only semantic
     *       difference is what a zero RelocationEntry::fromPatch means: for BKTR it selects the base
     *       RomFs, for Sparse it means "this virtual region was never written, read back as zero"
     * @url https://switchbrew.org/wiki/NCA#Sparse_Storage
     */
    class SparseStorage : public Backing {
      private:
        std::shared_ptr<Backing> backing; //!< The CTR-decrypted backing that physical offsets are read from, addressed relative to the section's start
        RelocationBlock block; //!< The L1 offset node of the sparse bucket tree
        std::vector<RelocationBucket> buckets; //!< The L2 entry nodes of the sparse bucket tree

        std::pair<u64, u64> GetEntryIndex(u64 offset);

        RelocationEntry GetEntry(u64 offset);

        RelocationEntry GetNextEntry(u64 offset);

      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override;

      public:
        /**
         * @param backing A backing covering the section's full physical (on-disk) range, already CTR-decrypted, addressed relative to the section's start
         * @param block The parsed L1 offset node of the sparse bucket tree
         * @param buckets The parsed L2 entry nodes of the sparse bucket tree
         * @param virtualSize The full virtual (uncompacted) size of the section, this becomes the exposed `Backing::size`
         */
        SparseStorage(std::shared_ptr<Backing> backing, RelocationBlock block, std::vector<RelocationBucket> buckets, u64 virtualSize);
    };
}
