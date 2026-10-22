// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <iterator>
#include "overlap_index.h"

namespace skyline::gpu::texture {
    // The primary mapping is handled by TextureManager's view/alias matcher.
    // Find previously cached textures touching secondary, possibly disjoint,
    // physical ranges before the new texture is materialized.
    // The augmented index handles nested intervals without scanning all entries.
    template<typename GuestMappings, typename MappingIndex, typename Visitor>
    void ForEachSecondaryMappingOverlap(const GuestMappings &guestMappings, const MappingIndex &index,
                                       const MappingOverlapIndex &overlaps, Visitor &&visit) {
        if (guestMappings.size() < 2 || index.empty())
            return;

        for (auto guest = std::next(guestMappings.begin()); guest != guestMappings.end(); ++guest) {
            if (guest->empty())
                continue;
            overlaps.ForEachOverlap(*guest, index, visit);
        }
    }
}
