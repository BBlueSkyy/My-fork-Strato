// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "backing.h"
#include "nca.h"

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
    std::shared_ptr<Backing> rawBacking; //!< The raw top-level NCA backing - data blocks are decrypted on demand, not upfront, since each needs a counter based on its *virtual* position
    crypto::KeyStore::Key128 key;
    std::array<u8, 0x10> ctr; //!< The section's standard (non-generation) upper IV
    size_t sectionPhysicalStart;
    RelocationBlock block;
    std::vector<RelocationBucket> buckets;
    u64 physicalBaseOffset;

    std::pair<u64, u64> GetEntryIndex(u64 offset);
    RelocationEntry GetEntry(u64 offset);
    RelocationEntry GetNextEntry(u64 offset);

  protected:
    size_t ReadImpl(span<u8> output, size_t offset) override;

  public:
    SparseStorage(std::shared_ptr<Backing> rawBacking, crypto::KeyStore::Key128 key, std::array<u8, 0x10> ctr, size_t sectionPhysicalStart,
                  RelocationBlock block, std::vector<RelocationBucket> buckets, u64 virtualSize, u64 physicalBaseOffset);
   };
}
