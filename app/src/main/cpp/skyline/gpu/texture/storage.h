// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <memory>
#include <utility>
#include <vector>
#include "copy_dependency.h"
#include "texture.h"

namespace skyline::gpu::texture {
    class TextureStorage;

    /**
     * @brief Groups storages whose complete guest ranges have a physical alias relationship
     *
     * Group membership and copy dependency state are metadata-only. Runtime registration
     * remains disabled until a concrete Vulkan transfer and storage-level read/write hooks
     * have both been verified for the relation.
     */
    class TextureGroup {
      private:
        std::vector<std::weak_ptr<TextureStorage>> storages;
        CopyDependencyTracker<TextureStorage> copyDependencies;

      public:
        void Attach(const std::shared_ptr<TextureStorage> &storage) {
            storages.emplace_back(storage);
        }

        const std::vector<std::weak_ptr<TextureStorage>> &GetStorages() const {
            return storages;
        }

        bool RegisterSynchronizedCopyDependency(const std::shared_ptr<TextureStorage> &backing,
                                                const TextureResourceLayout &backingLayout,
                                                const std::shared_ptr<TextureStorage> &requested,
                                                const TextureResourceLayout &requestedLayout,
                                                const ClassifiedResourceView &classified);
        bool MarkCopyRepresentationWritten(const std::shared_ptr<TextureStorage> &storage,
                                           std::span<const ResolvedSubresource> subresources);
        CopyRepresentationState GetCopyRepresentationState(
            const std::shared_ptr<TextureStorage> &storage, ResolvedSubresource subresource) const;
        PreparedDependencyRead<TextureStorage> PrepareCopyRepresentationRead(
            const std::shared_ptr<TextureStorage> &storage, ResolvedSubresource subresource) const;
        bool CompleteCopySynchronization(
            const PreparedDependencyRead<TextureStorage> &prepared);

        void MergeCopyDependenciesFrom(const TextureGroup &other) {
            copyDependencies.MergeFrom(other.copyDependencies);
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
        GuestResourceRanges ranges;

        TextureStorage(std::shared_ptr<Texture> texture, std::shared_ptr<TextureGroup> group, GuestResourceRanges ranges)
            : texture(std::move(texture)),
              group(std::move(group)),
              ranges(std::move(ranges)) {}
    };

    inline bool TextureGroup::RegisterSynchronizedCopyDependency(
        const std::shared_ptr<TextureStorage> &backing, const TextureResourceLayout &backingLayout,
        const std::shared_ptr<TextureStorage> &requested, const TextureResourceLayout &requestedLayout,
        const ClassifiedResourceView &classified) {
        return backing && requested && backing->group.get() == this && requested->group.get() == this &&
            copyDependencies.RegisterSynchronized(
                backing, backing->ranges, backingLayout,
                requested, requested->ranges, requestedLayout, classified);
    }

    inline bool TextureGroup::MarkCopyRepresentationWritten(
        const std::shared_ptr<TextureStorage> &storage,
        std::span<const ResolvedSubresource> subresources) {
        return copyDependencies.MarkWritten(storage, subresources);
    }

    inline CopyRepresentationState TextureGroup::GetCopyRepresentationState(
        const std::shared_ptr<TextureStorage> &storage, ResolvedSubresource subresource) const {
        return copyDependencies.GetState(storage, subresource);
    }

    inline PreparedDependencyRead<TextureStorage> TextureGroup::PrepareCopyRepresentationRead(
        const std::shared_ptr<TextureStorage> &storage, ResolvedSubresource subresource) const {
        return copyDependencies.PrepareRead(storage, subresource);
    }

    inline bool TextureGroup::CompleteCopySynchronization(
        const PreparedDependencyRead<TextureStorage> &prepared) {
        return copyDependencies.CompleteSynchronization(prepared);
    }

    inline std::shared_ptr<TextureStorage> CreateTextureStorage(std::shared_ptr<Texture> texture, GuestResourceRanges ranges) {
        auto group{std::make_shared<TextureGroup>()};
        auto storage{std::make_shared<TextureStorage>(std::move(texture), group, std::move(ranges))};
        group->Attach(storage);
        return storage;
    }

    /**
     * @brief Joins storages only when one complete ordered guest range is contained in the other
     *
     * This only records resource relationships. It deliberately does not synchronize,
     * copy, invalidate, or otherwise alter texture contents.
     */
    template<typename Range>
    inline void JoinTextureStorageGroups(const std::shared_ptr<TextureStorage> &storage, const Range &overlaps) {
        std::shared_ptr<TextureGroup> targetGroup{};

        for (const auto &overlap : overlaps) {
            if (overlap && overlap->group && IsCompleteGuestAlias(storage->ranges, overlap->ranges)) {
                targetGroup = overlap->group;
                break;
            }
        }

        if (!targetGroup)
            return;

        storage->group = targetGroup;
        targetGroup->Attach(storage);

        for (const auto &overlap : overlaps) {
            if (!overlap || !overlap->group || overlap->group == targetGroup ||
                !IsCompleteGuestAlias(storage->ranges, overlap->ranges))
                continue;

            auto sourceGroup{overlap->group};
            targetGroup->MergeCopyDependenciesFrom(*sourceGroup);
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
