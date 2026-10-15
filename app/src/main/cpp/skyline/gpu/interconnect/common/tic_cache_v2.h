// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace skyline::gpu::interconnect::tic_cache {
    // Shader type belongs in the identity: the same TIC may be interpreted as
    // e1D or e1DArray (and similarly for 2D/cube) by different shaders.
    template<typename Tic, typename ShaderType>
    struct Key {
        Tic tic{};
        ShaderType shaderType{};
        bool operator==(const Key &) const = default;
    };

    template<typename Tic, typename ShaderType, typename TicHash>
    struct KeyHash {
        std::size_t operator()(const Key<Tic, ShaderType> &key) const {
            std::size_t hash{TicHash{}(key.tic)};
            const auto typeHash{std::hash<ShaderType>{}(key.shaderType)};
            hash ^= typeHash + 0x9E3779B9U + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    /**
     * Separate the per-index fast lookup from shared, descriptor-keyed views.
     * The Store type is injected so production can retain robin_map while
     * standalone tests use unordered_map without Vulkan or dependencies.
     *
     * Index entries own their TextureView to prevent dangling pointers when
     * another TIC index replaces a descriptor-keyed global view.
     */
    template<typename Tic, typename ShaderType, typename View, typename Store>
    class Cache {
        using CacheKey = Key<Tic, ShaderType>;

        struct IndexEntry {
            CacheKey key{};
            std::shared_ptr<View> view{};
            std::uint64_t epoch{};
        };

        std::vector<IndexEntry> indexEntries;
        Store sharedViews;
        std::uint64_t epoch{1};
        std::uint64_t poolGeneration{};
        bool hasPool{};

        static bool Reusable(const std::shared_ptr<View> &view) {
            // master Texture has 'replaced'; no alias-retirement feature is
            // needed in this independent PR.
            return view && !view->texture->replaced;
        }

      public:
        // Only a physical TIC-pool mapping/extent change clears shared views.
        // Ordinary command submissions merely invalidate index-local results.
        void ObservePool(std::size_t size, std::uint64_t generation) {
            const bool changed{!hasPool || poolGeneration != generation || indexEntries.size() != size};
            if (!changed)
                return;

            if (hasPool && poolGeneration != generation)
                sharedViews.clear();
            indexEntries.assign(size, {});
            poolGeneration = generation;
            hasPool = true;
        }

        void InvalidateIndexed() {
            if (++epoch == 0) {
                epoch = 1;
                indexEntries.assign(indexEntries.size(), {});
            }
        }

        // Explicit Maxwell TIC-header invalidate occurs after the current
        // command submission. Cached descriptors must be resolved afresh.
        void InvalidateAll() {
            InvalidateIndexed();
            sharedViews.clear();
            indexEntries.assign(indexEntries.size(), {});
        }

        View *Lookup(std::size_t index, const Tic &tic, ShaderType shaderType) {
            if (index >= indexEntries.size())
                return {};

            const CacheKey key{tic, shaderType};
            auto &entry{indexEntries[index]};
            if (entry.epoch == epoch && entry.key == key && Reusable(entry.view))
                return entry.view.get();

            auto it{sharedViews.find(key)};
            if (it == sharedViews.end())
                return {};

            if (!Reusable(it->second)) {
                sharedViews.erase(it);
                return {};
            }

            entry = IndexEntry{key, it->second, epoch};
            return entry.view.get();
        }

        void Remember(std::size_t index, const Tic &tic, ShaderType shaderType,
                      const std::shared_ptr<View> &view) {
            if (!view || index >= indexEntries.size())
                return;
            const CacheKey key{tic, shaderType};
            sharedViews[key] = view;
            indexEntries[index] = IndexEntry{key, view, epoch};
        }

        std::size_t SharedViewCount() const {
            return sharedViews.size();
        }
    };
}
