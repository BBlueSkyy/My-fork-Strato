// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "guest_range.h"

namespace skyline::gpu::texture {
    class TextureStorage;

    /** Indexes each physical segment while retaining its position in its guest resource. */
    class TextureMappingCache {
      public:
        struct Mapping : GuestResourceRanges::Segment {
            std::shared_ptr<TextureStorage> storage;
        };

        struct LookupResult {
            std::vector<const Mapping *> firstMappingOverlaps; //!< Legacy match candidates, from highest address.
            std::vector<std::shared_ptr<TextureStorage>> storages; //!< Unique storages touching any query segment.
        };

      private:
        std::vector<Mapping> mappings; //!< Ordered by physical start address.

      public:
        LookupResult Lookup(const GuestResourceRanges &resource) const {
            LookupResult result{};
            if (!resource.Valid())
                return result;

            const auto &first{resource.Segments().front()};
            for (auto it{mappings.rbegin()}; it != mappings.rend(); ++it) {
                if (it->address < first.End() && first.address < it->End())
                    result.firstMappingOverlaps.push_back(&*it);
            }

            for (const auto &segment : resource.Segments()) {
                for (const auto &mapping : mappings) {
                    if (mapping.address >= segment.End())
                        break;
                    if (segment.address >= mapping.End())
                        continue;
                    if (std::find(result.storages.begin(), result.storages.end(), mapping.storage) == result.storages.end())
                        result.storages.push_back(mapping.storage);
                }
            }

            return result;
        }

        void Insert(const std::shared_ptr<TextureStorage> &storage, const GuestResourceRanges &resource) {
            if (!resource.Valid())
                return;

            for (const auto &segment : resource.Segments()) {
                auto position{std::upper_bound(mappings.begin(), mappings.end(), segment.address,
                    [](std::uintptr_t address, const Mapping &mapping) { return address < mapping.address; })};
                mappings.insert(position, Mapping{segment, storage});
            }
        }
    };
}
