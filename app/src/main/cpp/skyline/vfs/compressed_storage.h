// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <optional>

#include "backing.h"
#include "nca.h"

namespace skyline::vfs {
    /**
     * @brief Resolves an NCA section's "Compressed Storage" bucket tree, transparently LZ4-decompressing
     *        blocks on read
     * @note Confirmed against LibHac.Tools.FsSystem.CompressedStorage.Read(): each entry describes a block
     *       covering [VirtualOffset, nextEntry.VirtualOffset) that is either stored raw (None), implicitly
     *       zero-filled with no physical data at all (Zeroed), or LZ4-compressed in exactly PhysicalSize
     *       bytes at PhysicalOffset (Lz4) - the whole block is decompressed even if only part of it was
     *       requested, matching the reference implementation (no partial/streaming decompression)
     * @url https://switchbrew.org/wiki/NCA#CompressionInfo
     */
    class CompressedStorage : public Backing {
      private:
        std::shared_ptr<Backing> backing; //!< The CTR-decrypted backing that PhysicalOffset is read from directly - unlike Sparse, CompressionInfo has no separate physicalOffset base to add
        RelocationBlock block; //!< The L1 offset node of the bucket tree - entry-size independent, so the same struct as Sparse/BKTR's node storage is reused here
        std::vector<CompressedBucket> buckets; //!< The L2 entry nodes of the bucket tree

        std::optional<u64> cachedBlockVirtualOffset; //!< entry.virtualOffset of whichever Lz4 block is currently held in cachedBlock, if any
        std::vector<u8> cachedBlock; //!< The fully-decompressed bytes of the last Lz4 block read - callers routinely issue several small sequential reads against the same 64KB block (e.g. RomFS metadata parsing), so caching this avoids redoing a full block decompression for every one of them
        std::mutex cacheMutex; //!< Compressed storage may be read concurrently by multiple HOS threads

        std::pair<u64, u64> GetEntryIndex(u64 offset);

        CompressedEntry GetEntry(u64 offset);

        CompressedEntry GetNextEntry(u64 offset);

      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override;

      public:
        /**
         * @param backing A backing covering the section's full physical (on-disk) range, already CTR-decrypted, addressed relative to the section's start
         * @param block The parsed L1 offset node of the bucket tree
         * @param buckets The parsed L2 entry nodes of the bucket tree
         * @param virtualSize The full virtual (decompressed) size of the section, this becomes the exposed `Backing::size`
         */
        CompressedStorage(std::shared_ptr<Backing> backing, RelocationBlock block, std::vector<CompressedBucket> buckets, u64 virtualSize);
    };
}
