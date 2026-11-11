// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <common/mapping_identity.h>
#include <memory>
#include <utility>

namespace skyline::gpu::texture {
    // Translation association belongs to a descriptor cache entry, not a
    // physical Texture: distinct live IOVAs may legitimately share that image.
    template<typename View>
    struct MappingCacheEntry {
        std::shared_ptr<View> view;
        MappingIdentity::Snapshot mapping;

        bool Current() const { return view && !view->texture->replaced && mapping.IsCurrent(); }
        template<typename Retain>
        void Replace(std::shared_ptr<View> replacement, MappingIdentity::Snapshot receipt, Retain &&retain) {
            // Retained descriptor arrays/recording may still use raw View*.
            // Lifetime attachment can fail; do it before replacing either field.
            if (view && view != replacement) retain(view);
            view = std::move(replacement);
            mapping = std::move(receipt);
        }
    };
}
