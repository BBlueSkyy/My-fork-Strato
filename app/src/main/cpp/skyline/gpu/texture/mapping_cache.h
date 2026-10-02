// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "storage.h"

namespace skyline::gpu::texture {
    /**
     * @brief Spatial index for guest texture mappings.
     *
     * This class deliberately owns only range lookup and insertion ordering. Decisions
     * about format compatibility, views, synchronization and alias semantics remain in
     * TextureManager/TextureGroup.
     */
    class TextureMappingCache {
      public:
        /**
         * @brief A single contiguous guest CPU mapping associated with a texture storage
         */
        struct Mapping : span<u8> {
            std::shared_ptr<TextureStorage> storage;
            GuestTexture::Mappings::iterator iterator;

            template<typename... Args>
            Mapping(std::shared_ptr<TextureStorage> storage, GuestTexture::Mappings::iterator iterator, Args &&... args)
                : span<u8>(std::forward<Args>(args)...),
                  storage(std::move(storage)),
                  iterator(iterator) {}
        };

        struct LookupResult {
            boost::container::small_vector<const Mapping *, 8> overlaps;
            size_t insertionIndex{};
        };

      private:
        std::vector<Mapping> mappings;

      public:
        /**
         * @brief Finds mappings overlapping the supplied guest range.
         *
         * The traversal and insertion position intentionally preserve the legacy
         * TextureManager ordering while moving range indexing out of FindOrCreate.
         */
        LookupResult Lookup(span<u8> guestMapping) const {
            LookupResult result{};

            auto mappingEnd{std::upper_bound(mappings.begin(), mappings.end(), guestMapping, [guestMapping](const auto &, const auto &element) {
                return guestMapping.end() < element.end();
            })};
            auto hostMapping{std::lower_bound(mappingEnd, mappings.end(), guestMapping, [guestMapping](const auto &, const auto &element) {
                return guestMapping.begin() < element.end();
            })};

            result.insertionIndex = static_cast<size_t>(std::distance(mappings.begin(), mappingEnd));

            while (hostMapping != mappings.begin() && (--hostMapping)->end() > guestMapping.begin())
                result.overlaps.push_back(&*hostMapping);

            return result;
        }

        /**
         * @brief Inserts a mapping at the insertion point produced by Lookup()
         */
        void InsertAt(size_t index, std::shared_ptr<TextureStorage> storage, GuestTexture::Mappings::iterator iterator, span<u8> mapping) {
            mappings.emplace(mappings.begin() + static_cast<std::ptrdiff_t>(index), std::move(storage), iterator, mapping);
        }

        /**
         * @brief Inserts an additional mapping while retaining address ordering
         */
        void Insert(std::shared_ptr<TextureStorage> storage, GuestTexture::Mappings::iterator iterator, span<u8> mapping) {
            auto position{std::upper_bound(mappings.begin(), mappings.end(), mapping)};
            mappings.emplace(position, std::move(storage), iterator, mapping);
        }
    };
}
