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

    template<typename Representation>
    struct PreparedDependencyRead {
        CopyReadState state{CopyReadState::Untracked};
        std::shared_ptr<Representation> source{};
        ResolvedSubresource sourceSubresource{};
        ResolvedSubresource destinationSubresource{};
    };

    /**
     * Tracks validity between exact subresources of separate host representations.
     *
     * Edges prove semantic equivalence only. They do not describe or imply a Vulkan copy
     * capability in either direction.
     */
    template<typename Representation>
    class CopyDependencyTracker {
      private:
        struct Node {
            std::weak_ptr<Representation> representation;
        };

        struct Endpoint {
            std::size_t node{};
            ResolvedSubresource subresource{};
            std::uint64_t generation{};
        };

        struct Edge {
            std::size_t first{};
            std::size_t second{};
        };

        std::vector<Node> nodes;
        std::vector<Endpoint> endpoints;
        std::vector<Edge> edges;
        std::uint64_t nextGeneration{1};

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

        std::size_t FindOrAddNode(const std::shared_ptr<Representation> &representation) {
            if (const auto existing = FindNode(representation))
                return *existing;
            nodes.push_back({representation});
            return nodes.size() - 1;
        }

        std::optional<std::size_t> FindEndpoint(std::size_t node,
                                                ResolvedSubresource subresource) const {
            for (std::size_t index{}; index < endpoints.size(); ++index)
                if (endpoints[index].node == node && endpoints[index].subresource == subresource)
                    return index;
            return std::nullopt;
        }

        std::size_t AddEndpoint(std::size_t node, ResolvedSubresource subresource,
                                std::uint64_t generation) {
            endpoints.push_back({node, subresource, generation});
            return endpoints.size() - 1;
        }

        std::optional<std::size_t> FindEdge(std::size_t lhs, std::size_t rhs) const {
            for (std::size_t index{}; index < edges.size(); ++index)
                if ((edges[index].first == lhs && edges[index].second == rhs) ||
                    (edges[index].first == rhs && edges[index].second == lhs))
                    return index;
            return std::nullopt;
        }

        std::vector<std::size_t> Component(std::size_t root) const {
            std::vector<std::size_t> component;
            std::vector<bool> visited(endpoints.size());
            std::vector<std::size_t> pending{root};
            visited[root] = true;
            while (!pending.empty()) {
                const auto endpoint = pending.back();
                pending.pop_back();
                component.push_back(endpoint);
                for (const auto &edge : edges) {
                    std::optional<std::size_t> adjacent;
                    if (edge.first == endpoint)
                        adjacent = edge.second;
                    else if (edge.second == endpoint)
                        adjacent = edge.first;
                    if (adjacent && !visited[*adjacent]) {
                        visited[*adjacent] = true;
                        pending.push_back(*adjacent);
                    }
                }
            }
            return component;
        }

        std::uint64_t ComponentGeneration(std::size_t endpoint) const {
            std::uint64_t generation{};
            for (const auto member : Component(endpoint))
                generation = std::max(generation, endpoints[member].generation);
            return generation;
        }

        bool IsCurrent(std::size_t endpoint) const {
            return endpoints[endpoint].generation == ComponentGeneration(endpoint);
        }

        std::uint64_t AllocateGeneration() {
            return nextGeneration++;
        }

        void PromoteFrontier(const std::vector<std::size_t> &component,
                             std::uint64_t oldGeneration, std::uint64_t newGeneration) {
            for (const auto endpoint : component)
                if (endpoints[endpoint].generation == oldGeneration)
                    endpoints[endpoint].generation = newGeneration;
        }

        bool HasContradictoryEdge(std::size_t backingNode, ResolvedSubresource backing,
                                  std::size_t requestedNode, ResolvedSubresource requested) const {
            for (const auto &edge : edges) {
                const Endpoint *edgeBacking{};
                const Endpoint *edgeRequested{};
                if (endpoints[edge.first].node == backingNode &&
                    endpoints[edge.second].node == requestedNode) {
                    edgeBacking = &endpoints[edge.first];
                    edgeRequested = &endpoints[edge.second];
                } else if (endpoints[edge.second].node == backingNode &&
                           endpoints[edge.first].node == requestedNode) {
                    edgeBacking = &endpoints[edge.second];
                    edgeRequested = &endpoints[edge.first];
                }
                if (edgeBacking &&
                    (edgeBacking->subresource == backing || edgeRequested->subresource == requested) &&
                    !(edgeBacking->subresource == backing && edgeRequested->subresource == requested))
                    return true;
            }
            return false;
        }

        bool RegisterPair(std::size_t backingNode, ResolvedSubresource backing,
                          std::size_t requestedNode, ResolvedSubresource requested) {
            const auto backingEndpoint = FindEndpoint(backingNode, backing);
            const auto requestedEndpoint = FindEndpoint(requestedNode, requested);

            if (backingEndpoint && requestedEndpoint &&
                FindEdge(*backingEndpoint, *requestedEndpoint))
                return true;
            if (HasContradictoryEdge(backingNode, backing, requestedNode, requested))
                return false;
            if ((backingEndpoint && !IsCurrent(*backingEndpoint)) ||
                (requestedEndpoint && !IsCurrent(*requestedEndpoint)))
                return false;

            if (!backingEndpoint && !requestedEndpoint) {
                const auto generation = AllocateGeneration();
                const auto newBacking = AddEndpoint(backingNode, backing, generation);
                const auto newRequested = AddEndpoint(requestedNode, requested, generation);
                edges.push_back({newBacking, newRequested});
                return true;
            }

            if (!backingEndpoint) {
                const auto newBacking = AddEndpoint(
                    backingNode, backing, endpoints[*requestedEndpoint].generation);
                edges.push_back({newBacking, *requestedEndpoint});
                return true;
            }
            if (!requestedEndpoint) {
                const auto newRequested = AddEndpoint(
                    requestedNode, requested, endpoints[*backingEndpoint].generation);
                edges.push_back({*backingEndpoint, newRequested});
                return true;
            }

            const auto backingComponent = Component(*backingEndpoint);
            if (std::find(backingComponent.begin(), backingComponent.end(), *requestedEndpoint) !=
                backingComponent.end()) {
                edges.push_back({*backingEndpoint, *requestedEndpoint});
                return true;
            }

            const auto requestedComponent = Component(*requestedEndpoint);
            const auto backingGeneration = endpoints[*backingEndpoint].generation;
            const auto requestedGeneration = endpoints[*requestedEndpoint].generation;
            const auto mergeGeneration = AllocateGeneration();
            PromoteFrontier(backingComponent, backingGeneration, mergeGeneration);
            PromoteFrontier(requestedComponent, requestedGeneration, mergeGeneration);
            edges.push_back({*backingEndpoint, *requestedEndpoint});
            return true;
        }

        bool RegisterSynchronizedInPlace(const std::shared_ptr<Representation> &backing,
                                         const std::shared_ptr<Representation> &requested,
                                         const ResolvedCopyRegion &region) {
            const auto backingNode = FindOrAddNode(backing);
            const auto requestedNode = FindOrAddNode(requested);
            for (const auto &mapping : region.subresources)
                if (!RegisterPair(backingNode, mapping.backing, requestedNode, mapping.requested))
                    return false;
            return true;
        }

      public:
        bool RegisterSynchronized(const std::shared_ptr<Representation> &backing,
                                  const GuestResourceRanges &backingRanges,
                                  const TextureResourceLayout &backingLayout,
                                  const std::shared_ptr<Representation> &requested,
                                  const GuestResourceRanges &requestedRanges,
                                  const TextureResourceLayout &requestedLayout,
                                  const ClassifiedResourceView &classified) {
            if (!backing || !requested || backing == requested ||
                classified.relation != TextureViewCompatibility::CopyOnly ||
                classified.sharedView || !classified.copyRegion ||
                classified.copyRegion->subresources.empty() ||
                !IsCompleteGuestAlias(backingRanges, requestedRanges))
                return false;

            const auto &mappings = classified.copyRegion->subresources;
            for (std::size_t index{}; index < mappings.size(); ++index) {
                const auto &mapping = mappings[index];
                if (!ContainsSubresource(backingLayout, mapping.backing) ||
                    !ContainsSubresource(requestedLayout, mapping.requested))
                    return false;
                for (std::size_t previous{}; previous < index; ++previous)
                    if (mappings[previous].backing == mapping.backing ||
                        mappings[previous].requested == mapping.requested)
                        return false;
            }

            auto updated{*this};
            if (!updated.RegisterSynchronizedInPlace(backing, requested, *classified.copyRegion))
                return false;
            *this = std::move(updated);
            return true;
        }

        CopyRepresentationState GetState(
            const std::shared_ptr<Representation> &representation,
            ResolvedSubresource subresource) const {
            const auto node = FindNode(representation);
            if (!node)
                return CopyRepresentationState::Untracked;
            const auto endpoint = FindEndpoint(*node, subresource);
            if (!endpoint)
                return CopyRepresentationState::Untracked;
            return IsCurrent(*endpoint)
                ? CopyRepresentationState::Current : CopyRepresentationState::Stale;
        }

        bool MarkWritten(const std::shared_ptr<Representation> &representation,
                         std::span<const ResolvedSubresource> subresources) {
            const auto node = FindNode(representation);
            if (!node || subresources.empty())
                return false;

            std::vector<std::size_t> writtenEndpoints;
            writtenEndpoints.reserve(subresources.size());
            for (const auto subresource : subresources) {
                const auto endpoint = FindEndpoint(*node, subresource);
                if (!endpoint)
                    return false;
                if (std::find(writtenEndpoints.begin(), writtenEndpoints.end(), *endpoint) ==
                    writtenEndpoints.end())
                    writtenEndpoints.push_back(*endpoint);
            }

            const auto generation = AllocateGeneration();
            for (const auto endpoint : writtenEndpoints)
                endpoints[endpoint].generation = generation;
            return true;
        }

        PreparedDependencyRead<Representation> PrepareRead(
            const std::shared_ptr<Representation> &representation,
            ResolvedSubresource subresource) const {
            const auto node = FindNode(representation);
            if (!node)
                return {};
            const auto endpoint = FindEndpoint(*node, subresource);
            if (!endpoint)
                return {};
            if (IsCurrent(*endpoint))
                return {.state = CopyReadState::Current};

            const auto currentGeneration = ComponentGeneration(*endpoint);
            for (const auto &edge : edges) {
                std::optional<std::size_t> sourceEndpoint;
                if (edge.first == *endpoint)
                    sourceEndpoint = edge.second;
                else if (edge.second == *endpoint)
                    sourceEndpoint = edge.first;
                if (!sourceEndpoint || endpoints[*sourceEndpoint].generation != currentGeneration)
                    continue;
                auto source = nodes[endpoints[*sourceEndpoint].node].representation.lock();
                if (source)
                    return {
                        .state = CopyReadState::SynchronizationRequired,
                        .source = std::move(source),
                        .sourceSubresource = endpoints[*sourceEndpoint].subresource,
                        .destinationSubresource = subresource,
                    };
            }
            return {.state = CopyReadState::Unavailable};
        }

        bool CompleteSynchronization(
            const std::shared_ptr<Representation> &destination,
            ResolvedSubresource destinationSubresource,
            const std::shared_ptr<Representation> &source,
            ResolvedSubresource sourceSubresource) {
            const auto destinationNode = FindNode(destination);
            const auto sourceNode = FindNode(source);
            if (!destinationNode || !sourceNode)
                return false;
            const auto destinationEndpoint = FindEndpoint(*destinationNode, destinationSubresource);
            const auto sourceEndpoint = FindEndpoint(*sourceNode, sourceSubresource);
            if (!destinationEndpoint || !sourceEndpoint ||
                !FindEdge(*destinationEndpoint, *sourceEndpoint) || !IsCurrent(*sourceEndpoint))
                return false;
            endpoints[*destinationEndpoint].generation = endpoints[*sourceEndpoint].generation;
            return true;
        }

        std::size_t RelationCount() const {
            return edges.size();
        }

        // Transitional representation-wide entry points keep storage.h source-compatible
        // until its subresource wrappers are replaced after the graph behavior is complete.
        bool RegisterSynchronized(const std::shared_ptr<Representation> &,
                                  const GuestResourceRanges &,
                                  const std::shared_ptr<Representation> &,
                                  const GuestResourceRanges &,
                                  const ClassifiedResourceView &) {
            return false;
        }

        bool MarkWritten(const std::shared_ptr<Representation> &) {
            return false;
        }

        CopyRepresentationState GetState(const std::shared_ptr<Representation> &) const {
            return CopyRepresentationState::Untracked;
        }

        PreparedCopyRead<Representation> PrepareRead(
            const std::shared_ptr<Representation> &) const {
            return {};
        }

        bool CompleteSynchronization(const std::shared_ptr<Representation> &,
                                     const std::shared_ptr<Representation> &) {
            return false;
        }

        void MergeFrom(const CopyDependencyTracker &) {}
    };
}
