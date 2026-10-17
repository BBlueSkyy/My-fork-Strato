// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include "storage.h"

namespace skyline::gpu::texture {
    /** V2 all-segment index, adapted for V3. Caller holds the TextureManager lock. */
    class TextureMappingCache {
      public:
        struct Mapping : GuestResourceRanges::Segment {
            std::shared_ptr<TextureStorage> storage;
            std::uintptr_t prefixEnd{}; //!< Maximum physical end through this entry, including enclosing mappings.
        };

        struct LookupResult {
            std::vector<const Mapping *> firstMappingOverlaps; //!< Highest start address first; valid until Insert.
            std::vector<std::shared_ptr<TextureStorage>> storages; //!< Unique storages touching any query segment.
        };

      private:
        std::vector<Mapping> mappings; //!< Ordered by physical start address, insertion order for ties.

        template<typename Function>
        void ForEachOverlap(const GuestResourceRanges::Segment &segment, Function &&function) const {
            const auto end{std::lower_bound(mappings.begin(), mappings.end(), segment.End(),
                [](const Mapping &mapping, std::uintptr_t address) { return mapping.address < address; })};
            const auto begin{std::upper_bound(mappings.begin(), end, segment.address,
                [](std::uintptr_t address, const Mapping &mapping) { return address < mapping.prefixEnd; })};
            for (auto it{begin}; it != end; ++it)
                if (segment.address < it->End())
                    function(*it);
        }

      public:
        /** Retain physical slice-source precedence while visiting each storage once. */
        std::vector<std::shared_ptr<TextureStorage>> StoragesInMappingOrder() const {
            std::vector<std::shared_ptr<TextureStorage>> result;
            std::unordered_set<const TextureStorage *> seen;
            for (const auto &mapping : mappings)
                if (seen.insert(mapping.storage.get()).second)
                    result.push_back(mapping.storage);
            return result;
        }

        LookupResult Lookup(const GuestResourceRanges &resource) const {
            LookupResult result{};
            if (!resource.Valid()) return result;
            bool first{true};
            for (const auto &segment : resource.Segments()) {
                ForEachOverlap(segment, [&](const Mapping &mapping) {
                    if (first) result.firstMappingOverlaps.push_back(&mapping);
                    if (std::find(result.storages.begin(), result.storages.end(), mapping.storage) == result.storages.end())
                        result.storages.push_back(mapping.storage);
                });
                first = false;
            }
            std::reverse(result.firstMappingOverlaps.begin(), result.firstMappingOverlaps.end());
            return result;
        }

        void Insert(const std::shared_ptr<TextureStorage> &storage) {
            if (!storage || !storage->ranges.Valid()) return;
            const auto count{storage->ranges.Segments().size()};
            if (count > mappings.max_size() - mappings.size())
                throw std::length_error("Texture mapping index is too large");
            const auto required{mappings.size() + count};
            if (required > mappings.capacity()) {
                const auto grown{mappings.capacity() > mappings.max_size() / 2
                    ? mappings.max_size() : mappings.capacity() * 2};
                // Reserve once, before publishing any segment. Following inserts
                // and prefix updates cannot fail midway through this storage.
                mappings.reserve(std::max(required, grown));
            }
            for (const auto &segment : storage->ranges.Segments()) {
                auto position{std::upper_bound(mappings.begin(), mappings.end(), segment.address,
                    [](std::uintptr_t address, const Mapping &mapping) { return address < mapping.address; })};
                position = mappings.insert(position, Mapping{segment, storage, segment.End()});
                auto prefixEnd{position == mappings.begin() ? std::uintptr_t{} : std::prev(position)->prefixEnd};
                for (auto it{position}; it != mappings.end(); ++it) {
                    const auto previous{it->prefixEnd};
                    prefixEnd = std::max(prefixEnd, it->End());
                    it->prefixEnd = prefixEnd;
                    if (it != position && previous == prefixEnd) break;
                }
            }
        }
    };
}
