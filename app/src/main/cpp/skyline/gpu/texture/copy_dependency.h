// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "resource_layout.h"

namespace skyline::gpu::texture {
    enum class CopyRepresentationState : std::uint8_t { Untracked, Current, Stale };
    enum class CopyReadState : std::uint8_t { Untracked, Current, SynchronizationRequired, Unavailable };

    struct CopyTransferRegion {
        ResolvedViewBase source{};
        ResolvedViewBase destination{};
        std::uint32_t mipCount{}, layerCount{};
    };

    template<typename Representation>
    struct PreparedCopyRead {
        CopyReadState state{CopyReadState::Untracked};
        std::shared_ptr<Representation> source{};
        CopyTransferRegion region{};
    };

    /**
     * Tracks authority between separate host representations of a proven guest resource.
     *
     * RegisterSynchronized is intentionally the only way to create a relation: its caller
     * must have established that both representations contain equivalent data. This class
     * records synchronization obligations but never performs a Vulkan copy itself.
     */
    template<typename Representation>
    class CopyDependencyTracker {
      private:
        struct Node {
            std::weak_ptr<Representation> representation;
            std::uint64_t epoch{};
        };

        struct Relation {
            std::size_t backing{};
            std::size_t requested{};
            ResolvedCopyRegion region{};
        };

        std::vector<Node> nodes;
        std::vector<Relation> relations;
        std::uint64_t nextEpoch{};

        static bool SameOwner(const std::weak_ptr<Representation> &lhs,
                              const std::weak_ptr<Representation> &rhs) {
            return !lhs.owner_before(rhs) && !rhs.owner_before(lhs);
        }

        std::optional<std::size_t> FindNode(const std::weak_ptr<Representation> &representation) const {
            for (std::size_t index{}; index < nodes.size(); ++index)
                if (SameOwner(nodes[index].representation, representation))
                    return index;
            return std::nullopt;
        }

        std::optional<std::size_t> FindNode(const std::shared_ptr<Representation> &representation) const {
            return representation ? FindNode(std::weak_ptr<Representation>{representation}) : std::nullopt;
        }

        std::optional<std::size_t> FindRelation(std::size_t lhs, std::size_t rhs) const {
            for (std::size_t index{}; index < relations.size(); ++index) {
                const auto &relation = relations[index];
                if ((relation.backing == lhs && relation.requested == rhs) ||
                    (relation.backing == rhs && relation.requested == lhs))
                    return index;
            }
            return std::nullopt;
        }

        std::optional<std::size_t> RelationFor(std::size_t node) const {
            for (std::size_t index{}; index < relations.size(); ++index)
                if (relations[index].backing == node || relations[index].requested == node)
                    return index;
            return std::nullopt;
        }

        std::uint64_t CurrentEpoch(const Relation &relation) const {
            return std::max(nodes[relation.backing].epoch, nodes[relation.requested].epoch);
        }

      public:
        bool RegisterSynchronized(const std::shared_ptr<Representation> &backing,
                                  const GuestResourceRanges &backingRanges,
                                  const std::shared_ptr<Representation> &requested,
                                  const GuestResourceRanges &requestedRanges,
                                  const ClassifiedResourceView &classified) {
            if (!backing || !requested || backing == requested ||
                classified.relation != TextureViewCompatibility::CopyOnly || classified.sharedView ||
                !classified.copyRegion || !classified.copyRegion->mipCount ||
                !classified.copyRegion->layerCount ||
                !IsCompleteGuestAlias(backingRanges, requestedRanges) ||
                FindNode(backing) || FindNode(requested))
                return false;

            // Authority is representation-wide for now. Restricting each representation to
            // one dependency prevents writes to independent mip/layer aliases from being
            // collapsed into a false single authority. Those graphs remain on the legacy path
            // until validity is tracked per subresource.
            const auto synchronizedEpoch = ++nextEpoch;
            const auto backingNode = nodes.size();
            nodes.push_back({backing, synchronizedEpoch});
            const auto requestedNode = nodes.size();
            nodes.push_back({requested, synchronizedEpoch});
            relations.push_back({backingNode, requestedNode, *classified.copyRegion});
            return true;
        }

        bool MarkWritten(const std::shared_ptr<Representation> &representation) {
            const auto node = FindNode(representation);
            if (!node)
                return false;
            nodes[*node].epoch = ++nextEpoch;
            return true;
        }

        CopyRepresentationState GetState(const std::shared_ptr<Representation> &representation) const {
            const auto node = FindNode(representation);
            if (!node)
                return CopyRepresentationState::Untracked;
            const auto relation = RelationFor(*node);
            if (!relation)
                return CopyRepresentationState::Untracked;
            return nodes[*node].epoch == CurrentEpoch(relations[*relation])
                ? CopyRepresentationState::Current : CopyRepresentationState::Stale;
        }

        PreparedCopyRead<Representation> PrepareRead(const std::shared_ptr<Representation> &representation) const {
            const auto node = FindNode(representation);
            if (!node)
                return {};
            const auto relationIndex = RelationFor(*node);
            if (!relationIndex)
                return {};
            const auto &relation = relations[*relationIndex];
            const auto currentEpoch = CurrentEpoch(relation);
            if (nodes[*node].epoch == currentEpoch)
                return {.state = CopyReadState::Current};

            const bool isBacking = relation.backing == *node;
            const auto sourceNode = isBacking ? relation.requested : relation.backing;
            const auto source = nodes[sourceNode].representation.lock();
            if (!source || nodes[sourceNode].epoch != currentEpoch)
                return {.state = CopyReadState::Unavailable};

            return {.state = CopyReadState::SynchronizationRequired, .source = source,
                .region = {.source = isBacking ? relation.region.requested : relation.region.backing,
                           .destination = isBacking ? relation.region.backing : relation.region.requested,
                           .mipCount = relation.region.mipCount,
                           .layerCount = relation.region.layerCount}};
        }

        bool CompleteSynchronization(const std::shared_ptr<Representation> &destination,
                                     const std::shared_ptr<Representation> &source) {
            const auto destinationNode = FindNode(destination);
            const auto sourceNode = FindNode(source);
            if (!destinationNode || !sourceNode)
                return false;
            const auto relationIndex = FindRelation(*destinationNode, *sourceNode);
            if (!relationIndex || nodes[*sourceNode].epoch != CurrentEpoch(relations[*relationIndex]))
                return false;
            nodes[*destinationNode].epoch = nodes[*sourceNode].epoch;
            return true;
        }

        void MergeFrom(const CopyDependencyTracker &other) {
            nextEpoch = std::max(nextEpoch, other.nextEpoch);
            for (const auto &otherRelation : other.relations) {
                const auto &otherBacking = other.nodes[otherRelation.backing];
                const auto &otherRequested = other.nodes[otherRelation.requested];
                const auto backingNode = FindNode(otherBacking.representation);
                const auto requestedNode = FindNode(otherRequested.representation);

                if (backingNode && requestedNode) {
                    if (FindRelation(*backingNode, *requestedNode)) {
                        nodes[*backingNode].epoch = std::max(nodes[*backingNode].epoch, otherBacking.epoch);
                        nodes[*requestedNode].epoch = std::max(nodes[*requestedNode].epoch, otherRequested.epoch);
                    }
                    continue;
                }
                if (backingNode || requestedNode)
                    continue; // Preserve the one-dependency invariant and leave this relation untracked.

                const auto newBacking = nodes.size();
                nodes.push_back(otherBacking);
                const auto newRequested = nodes.size();
                nodes.push_back(otherRequested);
                relations.push_back({newBacking, newRequested, otherRelation.region});
            }
        }

        std::size_t RelationCount() const { return relations.size(); }
    };
}
