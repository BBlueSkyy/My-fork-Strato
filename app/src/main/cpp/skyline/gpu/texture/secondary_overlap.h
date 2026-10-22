// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <iterator>

namespace skyline::gpu::texture {
    // The primary mapping is handled by TextureManager's view/alias matcher.
    // Find previously cached textures touching secondary, possibly disjoint,
    // physical ranges before the new texture is materialized.
    // Do not binary-search by mapping end: the index is ordered by start and
    // nested entries do not have monotonic end addresses.
    template<typename GuestMappings, typename MappingIndex, typename Visitor>
    void ForEachSecondaryMappingOverlap(const GuestMappings &guestMappings, const MappingIndex &index, Visitor &&visit) {
        if (guestMappings.size() < 2 || index.empty())
            return;

        for (auto guest = std::next(guestMappings.begin()); guest != guestMappings.end(); ++guest) {
            if (guest->empty())
                continue;
            for (const auto &entry : index) {
                if (entry.begin() < guest->end() && entry.end() > guest->begin())
                    visit(entry);
            }
        }
    }
}
