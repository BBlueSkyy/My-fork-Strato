// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <memory>
#include <utility>
#include <vector>
#include "texture.h"

namespace skyline::gpu::texture {
    class TextureStorage;

    /**
     * @brief Groups host texture storages that represent overlapping/aliased guest memory
     *
     * Groups currently contain one storage. The grouping exists as a behavior-neutral
     * foundation for sharing validity/dirty state and explicit alias dependencies later.
     */
    class TextureGroup {
      private:
        std::vector<std::weak_ptr<TextureStorage>> storages;

      public:
        void Attach(const std::shared_ptr<TextureStorage> &storage) {
            storages.emplace_back(storage);
        }

        const std::vector<std::weak_ptr<TextureStorage>> &GetStorages() const {
            return storages;
        }
    };

    /**
     * @brief Transitional owner for a host texture representation
     *
     * The legacy Texture object still owns the Vulkan backing and synchronization state.
     * Moving that ownership here can therefore be done incrementally without changing
     * TextureView or the current synchronization behavior.
     */
    class TextureStorage {
      public:
        std::shared_ptr<Texture> texture;
        std::shared_ptr<TextureGroup> group;

        TextureStorage(std::shared_ptr<Texture> texture, std::shared_ptr<TextureGroup> group)
            : texture(std::move(texture)),
              group(std::move(group)) {}
    };

    inline std::shared_ptr<TextureStorage> CreateTextureStorage(std::shared_ptr<Texture> texture) {
        auto group{std::make_shared<TextureGroup>()};
        auto storage{std::make_shared<TextureStorage>(std::move(texture), group)};
        group->Attach(storage);
        return storage;
    }

    /**
     * @brief Places a newly created storage in the same alias group as overlapping storages
     *
     * This only records resource relationships. It deliberately does not synchronize,
     * copy, invalidate, or otherwise alter texture contents.
     */
    template<typename Range>
    inline void JoinTextureStorageGroups(const std::shared_ptr<TextureStorage> &storage, const Range &overlaps) {
        std::shared_ptr<TextureGroup> targetGroup{};

        for (const auto &overlap : overlaps) {
            if (overlap && overlap->group) {
                targetGroup = overlap->group;
                break;
            }
        }

        if (!targetGroup)
            return;

        storage->group = targetGroup;
        targetGroup->Attach(storage);

        for (const auto &overlap : overlaps) {
            if (!overlap || !overlap->group || overlap->group == targetGroup)
                continue;

            auto sourceGroup{overlap->group};
            for (const auto &weakStorage : sourceGroup->GetStorages()) {
                auto member{weakStorage.lock()};
                if (!member || member->group == targetGroup)
                    continue;

                member->group = targetGroup;
                targetGroup->Attach(member);
            }
        }
    }
}
