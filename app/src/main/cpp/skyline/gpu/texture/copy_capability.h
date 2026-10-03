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

    enum class CopySynchronizationState : std::uint8_t {
        Untracked,
        Current,
        SourceUnavailable,
        CapabilityUnavailable,
        Ready,
    };

    template<typename Representation>
    struct PreparedCopySynchronization {
        CopySynchronizationState state{CopySynchronizationState::Untracked};
        CopyCapability capability{CopyCapability::ExactImageCopy};
        PreparedDependencyRead<Representation> read{};
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
        };

        std::vector<Route> routes;

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
            ResolvedSubresource destinationSubresource) {
            if (sourceLayout.imageType != destinationLayout.imageType ||
                sourceLayout.imageType == ImageKind::ThreeDimensional ||
                sourceSubresource.depthSlice || destinationSubresource.depthSlice ||
                !sourceImage.hostFormat || sourceImage.hostFormat != destinationImage.hostFormat ||
                !sourceImage.aspectMask || sourceImage.aspectMask != destinationImage.aspectMask ||
                !std::has_single_bit(sourceImage.aspectMask) ||
                sourceImage.sampleCount != 1 || destinationImage.sampleCount != 1 ||
                !sourceImage.transferSource || !destinationImage.transferDestination)
                return false;

            const auto source = FindExactSubresource(sourceLayout, sourceSubresource);
            const auto destination = FindExactSubresource(destinationLayout, destinationSubresource);
            if (!source || !destination || !source->width || !source->height || source->depth != 1 ||
                !destination->width || !destination->height || destination->depth != 1 ||
                source->width != destination->width || source->height != destination->height)
                return false;

            return sourceLayout.imageType != ImageKind::OneDimensional || source->height == 1;
        }

        bool HasRoute(const std::shared_ptr<Representation> &source,
                      ResolvedSubresource sourceSubresource,
                      const std::shared_ptr<Representation> &destination,
                      ResolvedSubresource destinationSubresource,
                      CopyCapability capability = CopyCapability::ExactImageCopy) const {
            if (!source || !destination)
                return false;
            const std::weak_ptr<Representation> weakSource{source};
            const std::weak_ptr<Representation> weakDestination{destination};
            for (const auto &route : routes)
                if (route.capability == capability &&
                    SameOwner(route.source, weakSource) &&
                    SameOwner(route.destination, weakDestination) &&
                    route.sourceSubresource == sourceSubresource &&
                    route.destinationSubresource == destinationSubresource)
                    return true;
            return false;
        }

      public:
        bool RegisterExactImageCopy(
            const CopyDependencyTracker<Representation> &dependencies,
            const std::shared_ptr<Representation> &source,
            const TextureResourceLayout &sourceLayout, const CopyImageInfo &sourceImage,
            ResolvedSubresource sourceSubresource,
            const std::shared_ptr<Representation> &destination,
            const TextureResourceLayout &destinationLayout, const CopyImageInfo &destinationImage,
            ResolvedSubresource destinationSubresource) {
            if (!source || !destination || source == destination ||
                !dependencies.HasDirectRelation(
                    source, sourceSubresource, destination, destinationSubresource) ||
                !SupportsExactImageCopy(
                    sourceLayout, sourceImage, sourceSubresource,
                    destinationLayout, destinationImage, destinationSubresource))
                return false;

            if (HasRoute(source, sourceSubresource, destination, destinationSubresource))
                return true;
            routes.push_back({
                source, sourceSubresource, destination, destinationSubresource,
                CopyCapability::ExactImageCopy,
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
                    if (!HasRoute(read.source, read.sourceSubresource,
                                  read.destination, read.destinationSubresource))
                        return {.state = CopySynchronizationState::CapabilityUnavailable};
                    return {
                        .state = CopySynchronizationState::Ready,
                        .capability = CopyCapability::ExactImageCopy,
                        .read = std::move(read),
                    };
            }
            return {};
        }

        bool CompleteSynchronization(
            CopyDependencyTracker<Representation> &dependencies,
            const PreparedCopySynchronization<Representation> &prepared,
            bool executionSucceeded) const {
            if (!executionSucceeded || prepared.state != CopySynchronizationState::Ready ||
                prepared.capability != CopyCapability::ExactImageCopy ||
                !HasRoute(prepared.read.source, prepared.read.sourceSubresource,
                          prepared.read.destination, prepared.read.destinationSubresource))
                return false;
            return dependencies.CompleteSynchronization(prepared.read);
        }

        std::size_t RouteCount() const {
            return routes.size();
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
