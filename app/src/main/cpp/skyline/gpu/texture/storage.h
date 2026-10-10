// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "copy_capability.h"
#ifndef SKYLINE_TEXTURE_STORAGE_METADATA_ONLY
#include "texture.h"
#else
namespace skyline::gpu { class Texture; }
#endif

namespace skyline::gpu::texture {
    class TextureStorage;
    class PendingCopySynchronization;

    struct ScheduledCopySynchronization {
        CopySynchronizationState state{CopySynchronizationState::Untracked};
        std::shared_ptr<PendingCopySynchronization> pending;
    };

    /**
     * @brief Groups storages whose complete guest ranges have a physical alias relationship
     *
     * Runtime synchronization remains capability-gated and covers only explicitly
     * verified exact-image-copy relations.
     */
    class TextureGroup : public std::enable_shared_from_this<TextureGroup> {
      private:
        mutable std::mutex runtimeSynchronizationMutex;
        mutable std::mutex mutex;
        std::vector<std::weak_ptr<TextureStorage>> storages;
        CopyDependencyTracker<TextureStorage> copyDependencies;
        CopyCapabilityTracker<TextureStorage> copyCapabilities;

      public:
        /**
         * Serializes command executors only while they hold textures participating in
         * executable CopyOnly routes. This keeps source/destination texture locking
         * ordered without changing the global barrier policy.
         */
        std::mutex &RuntimeSynchronizationMutex() {
            return runtimeSynchronizationMutex;
        }

        void Attach(const std::shared_ptr<TextureStorage> &storage) {
            std::scoped_lock lock{mutex};
            storages.emplace_back(storage);
        }

        std::vector<std::weak_ptr<TextureStorage>> GetStorages() const {
            std::scoped_lock lock{mutex};
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
        bool RegisterExactImageCopyCapability(
            const std::shared_ptr<TextureStorage> &source,
            const TextureResourceLayout &sourceLayout, const CopyImageInfo &sourceImage,
            ResolvedSubresource sourceSubresource,
            const std::shared_ptr<TextureStorage> &destination,
            const TextureResourceLayout &destinationLayout, const CopyImageInfo &destinationImage,
            ResolvedSubresource destinationSubresource, bool supportsMaintenance5 = false);
        bool RegisterMaintenance5CopyOnly(
            const std::shared_ptr<TextureStorage> &backing,
            const TextureResourceLayout &backingLayout, const CopyImageInfo &backingImage,
            const std::shared_ptr<TextureStorage> &requested,
            const TextureResourceLayout &requestedLayout, const CopyImageInfo &requestedImage,
            const ClassifiedResourceView &classified);
        bool RegisterCubeCompatibleCopyOnly(
            const std::shared_ptr<TextureStorage> &backing,
            const TextureResourceLayout &backingLayout, const CopyImageInfo &backingImage,
            const std::shared_ptr<TextureStorage> &requested,
            const TextureResourceLayout &requestedLayout, const CopyImageInfo &requestedImage,
            const ClassifiedResourceView &classified, FormatCompatibility format);
        bool RegisterDepthSliceCopyOnly(
            const std::shared_ptr<TextureStorage> &backing,
            const TextureResourceLayout &backingLayout, const CopyImageInfo &backingImage,
            const std::shared_ptr<TextureStorage> &requested,
            const TextureResourceLayout &requestedLayout, const CopyImageInfo &requestedImage,
            const ClassifiedResourceView &classified, FormatCompatibility format);
        PreparedCopySynchronization<TextureStorage> PrepareCopySynchronization(
            const std::shared_ptr<TextureStorage> &destination,
            ResolvedSubresource destinationSubresource) const;
        bool BeginCopySynchronization(
            const PreparedCopySynchronization<TextureStorage> &prepared);
        ScheduledCopySynchronization ScheduleCopySynchronization(
            const std::shared_ptr<TextureStorage> &destination,
            ResolvedSubresource destinationSubresource);
        bool CompleteCopySynchronization(
            const PreparedCopySynchronization<TextureStorage> &prepared,
            bool executionSucceeded);
        bool HasExecutableCopyRoutes() const;
        bool TryMergeCopyDependenciesFrom(TextureGroup &other);
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
        GuestResourceRanges ranges;

      private:
        std::shared_ptr<TextureGroup> group;

      public:

        TextureStorage(std::shared_ptr<Texture> texture, std::shared_ptr<TextureGroup> group,
                       GuestResourceRanges ranges)
            : texture(std::move(texture)),
              ranges(std::move(ranges)),
              group(std::move(group)) {}

        std::shared_ptr<TextureGroup> GetGroup() const {
            return std::atomic_load_explicit(&group, std::memory_order_acquire);
        }

        void SetGroup(std::shared_ptr<TextureGroup> newGroup) {
            std::atomic_store_explicit(&group, std::move(newGroup), std::memory_order_release);
        }

        bool MoveFromGroup(const std::shared_ptr<TextureGroup> &source,
                           const std::shared_ptr<TextureGroup> &destination) {
            auto expected{source};
            return std::atomic_compare_exchange_strong_explicit(
                &group, &expected, destination,
                std::memory_order_acq_rel, std::memory_order_acquire);
        }
    };

    class PendingCopySynchronization {
      private:
        std::shared_ptr<TextureGroup> group;
        PreparedCopySynchronization<TextureStorage> prepared;
        std::atomic_flag completed{};

      public:
        PendingCopySynchronization(
            std::shared_ptr<TextureGroup> group,
            PreparedCopySynchronization<TextureStorage> prepared)
            : group(std::move(group)), prepared(std::move(prepared)) {}

        ~PendingCopySynchronization() {
            Complete(false);
        }

        PendingCopySynchronization(const PendingCopySynchronization &) = delete;
        PendingCopySynchronization &operator=(const PendingCopySynchronization &) = delete;

        const PreparedCopySynchronization<TextureStorage> &Prepared() const {
            return prepared;
        }

        bool Complete(bool executionSucceeded = true) {
            if (completed.test_and_set())
                return false;
            return group->CompleteCopySynchronization(prepared, executionSucceeded);
        }
    };

    inline bool TextureGroup::RegisterSynchronizedCopyDependency(
        const std::shared_ptr<TextureStorage> &backing, const TextureResourceLayout &backingLayout,
        const std::shared_ptr<TextureStorage> &requested, const TextureResourceLayout &requestedLayout,
        const ClassifiedResourceView &classified) {
        std::scoped_lock lock{mutex};
        return backing && requested && backing->GetGroup().get() == this &&
            requested->GetGroup().get() == this &&
            copyDependencies.RegisterSynchronized(
                backing, backing->ranges, backingLayout,
                requested, requested->ranges, requestedLayout, classified);
    }

    inline bool TextureGroup::MarkCopyRepresentationWritten(
        const std::shared_ptr<TextureStorage> &storage,
        std::span<const ResolvedSubresource> subresources) {
        std::scoped_lock lock{mutex};
        return storage && storage->GetGroup().get() == this &&
            copyDependencies.MarkWritten(storage, subresources);
    }

    inline CopyRepresentationState TextureGroup::GetCopyRepresentationState(
        const std::shared_ptr<TextureStorage> &storage, ResolvedSubresource subresource) const {
        std::scoped_lock lock{mutex};
        if (!storage || storage->GetGroup().get() != this)
            return CopyRepresentationState::Untracked;
        return copyDependencies.GetState(storage, subresource);
    }

    inline PreparedDependencyRead<TextureStorage> TextureGroup::PrepareCopyRepresentationRead(
        const std::shared_ptr<TextureStorage> &storage, ResolvedSubresource subresource) const {
        std::scoped_lock lock{mutex};
        if (!storage || storage->GetGroup().get() != this)
            return {};
        return copyDependencies.PrepareRead(storage, subresource);
    }

    inline bool TextureGroup::RegisterExactImageCopyCapability(
        const std::shared_ptr<TextureStorage> &source,
        const TextureResourceLayout &sourceLayout, const CopyImageInfo &sourceImage,
        ResolvedSubresource sourceSubresource,
        const std::shared_ptr<TextureStorage> &destination,
        const TextureResourceLayout &destinationLayout, const CopyImageInfo &destinationImage,
        ResolvedSubresource destinationSubresource, bool supportsMaintenance5) {
        std::scoped_lock lock{mutex};
        return source && destination && source->GetGroup().get() == this &&
            destination->GetGroup().get() == this &&
            copyCapabilities.RegisterExactImageCopy(
                copyDependencies,
                source, sourceLayout, sourceImage, sourceSubresource,
                destination, destinationLayout, destinationImage, destinationSubresource,
                supportsMaintenance5);
    }

    inline bool TextureGroup::RegisterMaintenance5CopyOnly(
        const std::shared_ptr<TextureStorage> &backing,
        const TextureResourceLayout &backingLayout, const CopyImageInfo &backingImage,
        const std::shared_ptr<TextureStorage> &requested,
        const TextureResourceLayout &requestedLayout, const CopyImageInfo &requestedImage,
        const ClassifiedResourceView &classified) {
        std::scoped_lock runtimeLock{runtimeSynchronizationMutex};
        std::scoped_lock lock{mutex};
        const bool dimensionalPair{
            (backingLayout.imageType == ImageKind::OneDimensional &&
             requestedLayout.imageType == ImageKind::TwoDimensional) ||
            (backingLayout.imageType == ImageKind::TwoDimensional &&
             requestedLayout.imageType == ImageKind::OneDimensional)
        };
        if (!backing || !requested || backing->GetGroup().get() != this ||
            requested->GetGroup().get() != this ||
            !dimensionalPair ||
            classified.relation != TextureViewCompatibility::CopyOnly ||
            !classified.copyRegion || classified.copyRegion->subresources.empty())
            return false;

        auto updatedDependencies{copyDependencies};
        if (!updatedDependencies.RegisterSynchronized(
                backing, backing->ranges, backingLayout,
                requested, requested->ranges, requestedLayout, classified))
            return false;

        auto updatedCapabilities{copyCapabilities};
        {
            auto forward{updatedCapabilities};
            bool complete{true};
            for (const auto &mapping : classified.copyRegion->subresources)
                complete &= forward.RegisterExactImageCopy(
                    updatedDependencies,
                    backing, backingLayout, backingImage, mapping.backing,
                    requested, requestedLayout, requestedImage, mapping.requested, true);
            if (complete) {
                updatedCapabilities = std::move(forward);
            } else
                return false;
        }
        {
            auto reverse{updatedCapabilities};
            bool complete{true};
            for (const auto &mapping : classified.copyRegion->subresources)
                complete &= reverse.RegisterExactImageCopy(
                    updatedDependencies,
                    requested, requestedLayout, requestedImage, mapping.requested,
                    backing, backingLayout, backingImage, mapping.backing, true);
            if (complete) {
                updatedCapabilities = std::move(reverse);
            } else
                return false;
        }

        copyDependencies = std::move(updatedDependencies);
        copyCapabilities = std::move(updatedCapabilities);
        return true;
    }

    inline bool TextureGroup::RegisterCubeCompatibleCopyOnly(
        const std::shared_ptr<TextureStorage> &backing,
        const TextureResourceLayout &backingLayout, const CopyImageInfo &backingImage,
        const std::shared_ptr<TextureStorage> &requested,
        const TextureResourceLayout &requestedLayout, const CopyImageInfo &requestedImage,
        const ClassifiedResourceView &classified, FormatCompatibility format) {
        std::scoped_lock runtimeLock{runtimeSynchronizationMutex};
        std::scoped_lock lock{mutex};
        const bool cubeView{requestedLayout.viewType == ViewKind::Cube ||
            requestedLayout.viewType == ViewKind::CubeArray};
        const bool validCubeLayers{requestedLayout.viewLayerBase % 6 == 0 &&
            (requestedLayout.viewType == ViewKind::Cube
                ? requestedLayout.viewLayerCount == 6
                : requestedLayout.viewLayerCount != 0 && requestedLayout.viewLayerCount % 6 == 0)};
        if (!backing || !requested || backing->GetGroup().get() != this ||
            requested->GetGroup().get() != this ||
            format != FormatCompatibility::Exact ||
            backingLayout.imageType != ImageKind::TwoDimensional ||
            requestedLayout.imageType != ImageKind::TwoDimensional ||
            backingLayout.cubeCompatible || !requestedLayout.cubeCompatible ||
            !cubeView || !validCubeLayers ||
            ClassifyTextureViewCompatibility(
                backingLayout, requestedLayout, format) != TextureViewCompatibility::CopyOnly ||
            classified.relation != TextureViewCompatibility::CopyOnly ||
            !classified.copyRegion || classified.copyRegion->subresources.empty())
            return false;

        auto updatedDependencies{copyDependencies};
        if (!updatedDependencies.RegisterSynchronized(
                backing, backing->ranges, backingLayout,
                requested, requested->ranges, requestedLayout, classified))
            return false;

        auto updatedCapabilities{copyCapabilities};
        {
            auto forward{updatedCapabilities};
            bool complete{true};
            for (const auto &mapping : classified.copyRegion->subresources)
                complete &= forward.RegisterExactImageCopy(
                    updatedDependencies,
                    backing, backingLayout, backingImage, mapping.backing,
                    requested, requestedLayout, requestedImage, mapping.requested, false);
            if (complete) {
                updatedCapabilities = std::move(forward);
            } else
                return false;
        }
        {
            auto reverse{updatedCapabilities};
            bool complete{true};
            for (const auto &mapping : classified.copyRegion->subresources)
                complete &= reverse.RegisterExactImageCopy(
                    updatedDependencies,
                    requested, requestedLayout, requestedImage, mapping.requested,
                    backing, backingLayout, backingImage, mapping.backing, false);
            if (complete) {
                updatedCapabilities = std::move(reverse);
            } else
                return false;
        }

        copyDependencies = std::move(updatedDependencies);
        copyCapabilities = std::move(updatedCapabilities);
        return true;
    }

    inline bool TextureGroup::RegisterDepthSliceCopyOnly(
        const std::shared_ptr<TextureStorage> &backing,
        const TextureResourceLayout &backingLayout, const CopyImageInfo &backingImage,
        const std::shared_ptr<TextureStorage> &requested,
        const TextureResourceLayout &requestedLayout, const CopyImageInfo &requestedImage,
        const ClassifiedResourceView &classified, FormatCompatibility format) {
        if (!backing || !requested)
            return false;
        const auto requestedGroup{requested->GetGroup()};
        if (!requestedGroup)
            return false;

        const auto registerLocked = [&](bool adoptRequested) {
            const auto verified{ClassifyAndResolveView(
                backingLayout, requestedLayout, format, false)};
            if (backing->GetGroup().get() != this ||
                requested->GetGroup() != requestedGroup ||
                (!adoptRequested && requestedGroup.get() != this) ||
                format != FormatCompatibility::Exact ||
                verified.relation != TextureViewCompatibility::CopyOnly ||
                !verified.copyRegion || verified.copyRegion->subresources.size() != 1 ||
                classified.relation != TextureViewCompatibility::CopyOnly ||
                !classified.copyRegion ||
                classified.copyRegion->subresources != verified.copyRegion->subresources)
                return false;

            if (adoptRequested) {
                bool foundRequested{};
                for (const auto &weakStorage : requestedGroup->storages) {
                    const auto member{weakStorage.lock()};
                    if (!member)
                        continue;
                    if (member != requested)
                        return false;
                    foundRequested = true;
                }
                if (!foundRequested || requestedGroup->copyDependencies.RelationCount() ||
                    requestedGroup->copyCapabilities.RouteCount() ||
                    requestedGroup->copyCapabilities.HasPendingSynchronizations())
                    return false;
            }

            const auto &mapping{verified.copyRegion->subresources.front()};
            if (!IsExactBlockLinearDepthSliceRelation(
                    backingLayout, mapping.backing,
                    requestedLayout, mapping.requested))
                return false;

            auto updatedDependencies{copyDependencies};
            if (!updatedDependencies.RegisterSynchronized(
                    backing, backing->ranges, backingLayout,
                    requested, requested->ranges, requestedLayout, verified))
                return false;

            auto forward{copyCapabilities};
            if (!forward.RegisterExactImageCopy(
                    updatedDependencies,
                    backing, backingLayout, backingImage, mapping.backing,
                    requested, requestedLayout, requestedImage, mapping.requested, false))
                return false;

            auto reverse{forward};
            if (!reverse.RegisterExactImageCopy(
                    updatedDependencies,
                    requested, requestedLayout, requestedImage, mapping.requested,
                    backing, backingLayout, backingImage, mapping.backing, false))
                return false;

            if (adoptRequested) {
                const auto destinationGroup{shared_from_this()};
                if (!requested->MoveFromGroup(requestedGroup, destinationGroup))
                    return false;
                storages.emplace_back(requested);
            }
            copyDependencies = std::move(updatedDependencies);
            copyCapabilities = std::move(reverse);
            return true;
        };

        // The metadata mutexes serialize route publication. TextureManager may reach
        // this while its executor already owns this group's runtime serialization lock.
        if (requestedGroup.get() == this) {
            std::scoped_lock lock{mutex};
            return registerLocked(false);
        }

        std::scoped_lock lock{mutex, requestedGroup->mutex};
        return registerLocked(true);
    }

    inline PreparedCopySynchronization<TextureStorage> TextureGroup::PrepareCopySynchronization(
        const std::shared_ptr<TextureStorage> &destination,
        ResolvedSubresource destinationSubresource) const {
        std::scoped_lock lock{mutex};
        if (!destination || destination->GetGroup().get() != this)
            return {};
        return copyCapabilities.PrepareSynchronization(
            copyDependencies, destination, destinationSubresource);
    }

    inline bool TextureGroup::BeginCopySynchronization(
        const PreparedCopySynchronization<TextureStorage> &prepared) {
        std::scoped_lock lock{mutex};
        if (!prepared.read.source || !prepared.read.destination ||
            prepared.read.source->GetGroup().get() != this ||
            prepared.read.destination->GetGroup().get() != this)
            return false;
        return copyCapabilities.BeginSynchronization(prepared);
    }

    inline ScheduledCopySynchronization TextureGroup::ScheduleCopySynchronization(
        const std::shared_ptr<TextureStorage> &destination,
        ResolvedSubresource destinationSubresource) {
        std::scoped_lock lock{mutex};
        if (!destination || destination->GetGroup().get() != this)
            return {};

        auto prepared{copyCapabilities.PrepareSynchronization(
            copyDependencies, destination, destinationSubresource)};
        if (prepared.state != CopySynchronizationState::Ready)
            return {.state = prepared.state, .pending = {}};
        if (!copyCapabilities.BeginSynchronization(prepared))
            return {.state = CopySynchronizationState::Pending, .pending = {}};
        return {
            .state = CopySynchronizationState::Ready,
            .pending = std::make_shared<PendingCopySynchronization>(
                shared_from_this(), std::move(prepared)),
        };
    }

    inline bool TextureGroup::CompleteCopySynchronization(
        const PreparedCopySynchronization<TextureStorage> &prepared,
        bool executionSucceeded) {
        std::scoped_lock lock{mutex};
        if (!prepared.read.source || !prepared.read.destination ||
            prepared.read.source->GetGroup().get() != this ||
            prepared.read.destination->GetGroup().get() != this)
            return false;
        return copyCapabilities.CompleteSynchronization(
            copyDependencies, prepared, executionSucceeded);
    }

    inline bool TextureGroup::HasExecutableCopyRoutes() const {
        std::scoped_lock lock{mutex};
        return copyCapabilities.RouteCount() != 0;
    }

    inline bool TextureGroup::TryMergeCopyDependenciesFrom(TextureGroup &other) {
        if (this == &other)
            return true;
        std::scoped_lock runtimeLock{runtimeSynchronizationMutex, other.runtimeSynchronizationMutex};
        std::scoped_lock lock{mutex, other.mutex};
        if (copyCapabilities.HasPendingSynchronizations() ||
            other.copyCapabilities.HasPendingSynchronizations())
            return false;

        copyDependencies.MergeFrom(other.copyDependencies);
        copyCapabilities.MergeFrom(other.copyCapabilities);
        const auto sourceGroup{other.shared_from_this()};
        const auto destinationGroup{shared_from_this()};
        for (const auto &weakStorage : other.storages) {
            auto member{weakStorage.lock()};
            if (!member || !member->MoveFromGroup(sourceGroup, destinationGroup))
                continue;
            storages.emplace_back(member);
        }
        return true;
    }

#ifndef SKYLINE_TEXTURE_STORAGE_METADATA_ONLY
    inline std::shared_ptr<TextureStorage> CreateTextureStorage(
        std::shared_ptr<Texture> texture, GuestResourceRanges ranges) {
        auto group{std::make_shared<TextureGroup>()};
        auto storage{std::make_shared<TextureStorage>(
            std::move(texture), group, std::move(ranges))};
        if (storage->texture)
            storage->texture->storage = storage;
        group->Attach(storage);
        return storage;
    }
#endif

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
            auto overlapGroup{overlap ? overlap->GetGroup() : nullptr};
            if (overlapGroup && IsCompleteGuestAlias(storage->ranges, overlap->ranges)) {
                targetGroup = std::move(overlapGroup);
                break;
            }
        }

        if (!targetGroup)
            return;

        storage->SetGroup(targetGroup);
        targetGroup->Attach(storage);

        for (const auto &overlap : overlaps) {
            auto sourceGroup{overlap ? overlap->GetGroup() : nullptr};
            if (!sourceGroup || sourceGroup == targetGroup ||
                !IsCompleteGuestAlias(storage->ranges, overlap->ranges))
                continue;
            targetGroup->TryMergeCopyDependenciesFrom(*sourceGroup);
        }
    }
}
