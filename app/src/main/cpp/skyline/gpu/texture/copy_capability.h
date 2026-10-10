// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <bit>
#include <cstdint>
#include <memory>
#include <vector>
#include "copy_dependency.h"

namespace skyline::gpu::texture {
    enum class CopyCapability : std::uint8_t { ExactImageCopy };

    /** Host-image properties needed to validate one directional exact copy. */
    struct CopyImageInfo {
        std::uint64_t hostFormat{};
        std::uint32_t aspectMask{};
        std::uint32_t sampleCount{1};
        bool transferSource{};
        bool transferDestination{};
    };

    struct ExactImageCopyRegion {
        ResolvedSubresource sourceSubresource{};
        ResolvedSubresource destinationSubresource{};
        ImageKind sourceImageType{};
        ImageKind destinationImageType{};
        std::uint32_t sourceOffsetZ{}, destinationOffsetZ{};
        std::uint32_t width{}, height{}, depth{};
        std::uint32_t aspectMask{};
    };

    enum class CopySynchronizationState : std::uint8_t {
        Untracked,
        Current,
        SourceUnavailable,
        CapabilityUnavailable,
        Pending,
        Ready,
    };

    template<typename Representation>
    struct PreparedCopySynchronization {
        CopySynchronizationState state{CopySynchronizationState::Untracked};
        CopyCapability capability{CopyCapability::ExactImageCopy};
        PreparedDependencyRead<Representation> read{};
        ExactImageCopyRegion copyRegion{};
    };

    /**
     * Stores executable copy capabilities separately from semantic equivalence edges.
     *
     * Every route is directional. This class only prepares and completes metadata; it
     * neither records nor submits Vulkan commands.
     */
    template<typename Representation>
    class CopyCapabilityTracker {
      private:
        struct Route {
            std::weak_ptr<Representation> source;
            ResolvedSubresource sourceSubresource{};
            std::weak_ptr<Representation> destination;
            ResolvedSubresource destinationSubresource{};
            CopyCapability capability{};
            ExactImageCopyRegion copyRegion{};
        };

        struct PendingSynchronization {
            std::weak_ptr<Representation> source;
            ResolvedSubresource sourceSubresource{};
            std::weak_ptr<Representation> destination;
            ResolvedSubresource destinationSubresource{};
            std::uint64_t sourceGeneration{};
            std::uint64_t destinationGeneration{};
        };

        std::vector<Route> routes;
        std::vector<PendingSynchronization> pendingSynchronizations;

        static bool SameOwner(const std::weak_ptr<Representation> &lhs,
                              const std::weak_ptr<Representation> &rhs) {
            return !lhs.owner_before(rhs) && !rhs.owner_before(lhs);
        }

        static const GuestSubresource *FindExactSubresource(
            const TextureResourceLayout &layout, ResolvedSubresource resolved) {
            const GuestSubresource *result{};
            for (const auto &subresource : layout.subresources) {
                if (subresource.mip != resolved.mip || subresource.layer != resolved.layer)
                    continue;
                if (result)
                    return nullptr;
                result = &subresource;
            }
            return result;
        }

        static bool SupportsExactImageCopy(
            const TextureResourceLayout &sourceLayout, const CopyImageInfo &sourceImage,
            ResolvedSubresource sourceSubresource,
            const TextureResourceLayout &destinationLayout, const CopyImageInfo &destinationImage,
            ResolvedSubresource destinationSubresource, bool supportsMaintenance5) {
            const bool dimensionalCopy{
                (sourceLayout.imageType == ImageKind::OneDimensional &&
                 destinationLayout.imageType == ImageKind::TwoDimensional) ||
                (sourceLayout.imageType == ImageKind::TwoDimensional &&
                 destinationLayout.imageType == ImageKind::OneDimensional)
            };
            const bool depthSliceCopy{
                (sourceLayout.imageType == ImageKind::ThreeDimensional &&
                 destinationLayout.imageType == ImageKind::TwoDimensional) ||
                (sourceLayout.imageType == ImageKind::TwoDimensional &&
                 destinationLayout.imageType == ImageKind::ThreeDimensional)
            };
            if ((sourceLayout.imageType != destinationLayout.imageType &&
                 !depthSliceCopy && (!dimensionalCopy || !supportsMaintenance5)) ||
                ((sourceLayout.imageType == ImageKind::ThreeDimensional ||
                  destinationLayout.imageType == ImageKind::ThreeDimensional) &&
                 !depthSliceCopy) ||
                (!depthSliceCopy &&
                 (sourceSubresource.depthSlice || destinationSubresource.depthSlice)) ||
                !sourceImage.hostFormat || sourceImage.hostFormat != destinationImage.hostFormat ||
                !sourceImage.aspectMask || sourceImage.aspectMask != destinationImage.aspectMask ||
                !std::has_single_bit(sourceImage.aspectMask) ||
                sourceImage.sampleCount != 1 || destinationImage.sampleCount != 1 ||
                !sourceImage.transferSource || !destinationImage.transferDestination)
                return false;

            const auto source = FindExactSubresource(sourceLayout, sourceSubresource);
            const auto destination = FindExactSubresource(destinationLayout, destinationSubresource);
            if (!source || !destination || !source->width || !source->height ||
                !destination->width || !destination->height ||
                source->width != destination->width || source->height != destination->height)
                return false;

            if (depthSliceCopy)
                return IsExactBlockLinearDepthSliceRelation(
                    sourceLayout, sourceSubresource,
                    destinationLayout, destinationSubresource);

            if (source->depth != 1 || destination->depth != 1)
                return false;

            return (!dimensionalCopy && sourceLayout.imageType != ImageKind::OneDimensional) ||
                source->height == 1;
        }

        const Route *FindRoute(const std::shared_ptr<Representation> &source,
                               ResolvedSubresource sourceSubresource,
                               const std::shared_ptr<Representation> &destination,
                               ResolvedSubresource destinationSubresource,
                               CopyCapability capability = CopyCapability::ExactImageCopy) const {
            if (!source || !destination)
                return nullptr;
            const std::weak_ptr<Representation> weakSource{source};
            const std::weak_ptr<Representation> weakDestination{destination};
            for (const auto &route : routes)
                if (route.capability == capability &&
                    SameOwner(route.source, weakSource) &&
                    SameOwner(route.destination, weakDestination) &&
                    route.sourceSubresource == sourceSubresource &&
                    route.destinationSubresource == destinationSubresource)
                    return &route;
            return nullptr;
        }

        bool IsPending(const PreparedDependencyRead<Representation> &read) const {
            const std::weak_ptr<Representation> weakSource{read.source};
            const std::weak_ptr<Representation> weakDestination{read.destination};
            return std::any_of(pendingSynchronizations.begin(), pendingSynchronizations.end(),
                [&](const PendingSynchronization &pending) {
                    return SameOwner(pending.source, weakSource) &&
                        SameOwner(pending.destination, weakDestination) &&
                        pending.sourceSubresource == read.sourceSubresource &&
                        pending.destinationSubresource == read.destinationSubresource &&
                        pending.sourceGeneration == read.sourceGeneration &&
                        pending.destinationGeneration == read.destinationGeneration;
                });
        }

        auto FindPending(const PreparedDependencyRead<Representation> &read) {
            const std::weak_ptr<Representation> weakSource{read.source};
            const std::weak_ptr<Representation> weakDestination{read.destination};
            return std::find_if(pendingSynchronizations.begin(), pendingSynchronizations.end(),
                [&](const PendingSynchronization &pending) {
                    return SameOwner(pending.source, weakSource) &&
                        SameOwner(pending.destination, weakDestination) &&
                        pending.sourceSubresource == read.sourceSubresource &&
                        pending.destinationSubresource == read.destinationSubresource &&
                        pending.sourceGeneration == read.sourceGeneration &&
                        pending.destinationGeneration == read.destinationGeneration;
                });
        }

      public:
        /**
         * @brief Validates a potential CopyCompatible image copy without registering it
         *
         * The format classifier is the authority for the Vulkan compatibility class.
         * This method only accepts the current executor's exact-extent 1D/2D subset;
         * route registration remains intentionally disabled for this checkpoint.
         */
        static bool SupportsCopyCompatibleImageCopy(
            const TextureResourceLayout &sourceLayout, const CopyImageInfo &sourceImage,
            ResolvedSubresource sourceSubresource,
            const TextureResourceLayout &destinationLayout, const CopyImageInfo &destinationImage,
            ResolvedSubresource destinationSubresource, FormatCompatibility format) {
            constexpr std::uint32_t ColorAspectMask{1}; // VK_IMAGE_ASPECT_COLOR_BIT
            if (format != FormatCompatibility::CopyCompatible ||
                sourceLayout.imageType != destinationLayout.imageType ||
                sourceLayout.imageType == ImageKind::ThreeDimensional ||
                sourceSubresource.depthSlice || destinationSubresource.depthSlice ||
                !sourceImage.hostFormat || !destinationImage.hostFormat ||
                sourceImage.hostFormat == destinationImage.hostFormat ||
                sourceImage.aspectMask != ColorAspectMask ||
                destinationImage.aspectMask != ColorAspectMask ||
                sourceImage.sampleCount != 1 || destinationImage.sampleCount != 1 ||
                !sourceImage.transferSource || !destinationImage.transferDestination)
                return false;

            const auto source = FindExactSubresource(sourceLayout, sourceSubresource);
            const auto destination = FindExactSubresource(destinationLayout, destinationSubresource);
            if (!source || !destination || !source->width || !source->height ||
                !destination->width || !destination->height ||
                source->width != destination->width ||
                source->height != destination->height ||
                source->depth != 1 || destination->depth != 1)
                return false;

            return sourceLayout.imageType != ImageKind::OneDimensional ||
                source->height == 1;
        }

        bool RegisterExactImageCopy(
            const CopyDependencyTracker<Representation> &dependencies,
            const std::shared_ptr<Representation> &source,
            const TextureResourceLayout &sourceLayout, const CopyImageInfo &sourceImage,
            ResolvedSubresource sourceSubresource,
            const std::shared_ptr<Representation> &destination,
            const TextureResourceLayout &destinationLayout, const CopyImageInfo &destinationImage,
            ResolvedSubresource destinationSubresource, bool supportsMaintenance5 = false) {
            if (!source || !destination || source == destination ||
                !dependencies.HasDirectRelation(
                    source, sourceSubresource, destination, destinationSubresource) ||
                !SupportsExactImageCopy(
                    sourceLayout, sourceImage, sourceSubresource,
                    destinationLayout, destinationImage, destinationSubresource,
                    supportsMaintenance5))
                return false;

            if (FindRoute(source, sourceSubresource, destination, destinationSubresource))
                return true;
            const auto sourceDescription = FindExactSubresource(sourceLayout, sourceSubresource);
            routes.push_back({
                source, sourceSubresource, destination, destinationSubresource,
                CopyCapability::ExactImageCopy,
                {
                    .sourceSubresource = sourceSubresource,
                    .destinationSubresource = destinationSubresource,
                    .sourceImageType = sourceLayout.imageType,
                    .destinationImageType = destinationLayout.imageType,
                    .sourceOffsetZ = sourceLayout.imageType == ImageKind::ThreeDimensional
                        ? sourceSubresource.depthSlice : 0,
                    .destinationOffsetZ = destinationLayout.imageType == ImageKind::ThreeDimensional
                        ? destinationSubresource.depthSlice : 0,
                    .width = sourceDescription->width,
                    .height = sourceDescription->height,
                    .depth = 1,
                    .aspectMask = sourceImage.aspectMask,
                },
            });
            return true;
        }

        PreparedCopySynchronization<Representation> PrepareSynchronization(
            const CopyDependencyTracker<Representation> &dependencies,
            const std::shared_ptr<Representation> &destination,
            ResolvedSubresource destinationSubresource) const {
            auto read = dependencies.PrepareRead(destination, destinationSubresource);
            switch (read.state) {
                case CopyReadState::Untracked:
                    return {};
                case CopyReadState::Current:
                    return {.state = CopySynchronizationState::Current};
                case CopyReadState::Unavailable:
                    return {.state = CopySynchronizationState::SourceUnavailable};
                case CopyReadState::SynchronizationRequired:
                    const auto route = FindRoute(read.source, read.sourceSubresource,
                                                 read.destination, read.destinationSubresource);
                    if (!route)
                        return {.state = CopySynchronizationState::CapabilityUnavailable};
                    if (IsPending(read))
                        return {.state = CopySynchronizationState::Pending};
                    return {
                        .state = CopySynchronizationState::Ready,
                        .capability = CopyCapability::ExactImageCopy,
                        .read = std::move(read),
                        .copyRegion = route->copyRegion,
                    };
            }
            return {};
        }

        bool BeginSynchronization(
            const PreparedCopySynchronization<Representation> &prepared) {
            if (prepared.state != CopySynchronizationState::Ready ||
                prepared.capability != CopyCapability::ExactImageCopy ||
                !FindRoute(prepared.read.source, prepared.read.sourceSubresource,
                           prepared.read.destination, prepared.read.destinationSubresource) ||
                IsPending(prepared.read))
                return false;
            pendingSynchronizations.push_back({
                prepared.read.source, prepared.read.sourceSubresource,
                prepared.read.destination, prepared.read.destinationSubresource,
                prepared.read.sourceGeneration, prepared.read.destinationGeneration,
            });
            return true;
        }

        bool CompleteSynchronization(
            CopyDependencyTracker<Representation> &dependencies,
            const PreparedCopySynchronization<Representation> &prepared,
            bool executionSucceeded) {
            if (prepared.state != CopySynchronizationState::Ready ||
                prepared.capability != CopyCapability::ExactImageCopy ||
                !FindRoute(prepared.read.source, prepared.read.sourceSubresource,
                           prepared.read.destination, prepared.read.destinationSubresource))
                return false;
            const auto pending = FindPending(prepared.read);
            if (pending == pendingSynchronizations.end())
                return false;
            pendingSynchronizations.erase(pending);
            if (!executionSucceeded)
                return false;
            return dependencies.CompleteSynchronization(prepared.read);
        }

        std::size_t RouteCount() const {
            return routes.size();
        }

        bool HasPendingSynchronizations() const {
            return !pendingSynchronizations.empty();
        }

        void MergeFrom(const CopyCapabilityTracker &other) {
            for (const auto &route : other.routes) {
                bool found{};
                for (const auto &existing : routes)
                    if (existing.capability == route.capability &&
                        SameOwner(existing.source, route.source) &&
                        SameOwner(existing.destination, route.destination) &&
                        existing.sourceSubresource == route.sourceSubresource &&
                        existing.destinationSubresource == route.destinationSubresource) {
                        found = true;
                        break;
                    }
                if (!found)
                    routes.push_back(route);
            }
        }
    };
}
