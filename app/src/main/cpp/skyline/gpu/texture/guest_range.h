// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace skyline::gpu::texture {
    /** Guest physical ranges in the order they appear in a single GPU virtual resource. */
    class GuestResourceRanges {
      public:
        struct Segment {
            std::uintptr_t address{};
            std::size_t size{};
            std::size_t logicalOffset{};

            std::uintptr_t End() const { return address + size; }
        };

      private:
        std::vector<Segment> segments;
        std::size_t totalSize{};
        bool valid{true};

        const Segment *AtOffset(std::size_t offset) const {
            for (const auto &segment : segments)
                if (offset >= segment.logicalOffset && offset - segment.logicalOffset < segment.size)
                    return &segment;
            return nullptr;
        }

        bool MatchesAt(std::size_t offset, const GuestResourceRanges &other) const {
            for (std::size_t compared{}; compared < other.totalSize;) {
                const auto *lhs{AtOffset(offset + compared)};
                const auto *rhs{other.AtOffset(compared)};
                if (!lhs || !rhs)
                    return false;

                const auto lhsPosition{offset + compared - lhs->logicalOffset};
                const auto rhsPosition{compared - rhs->logicalOffset};
                if (lhs->address + lhsPosition != rhs->address + rhsPosition)
                    return false;

                compared += std::min({lhs->size - lhsPosition, rhs->size - rhsPosition, other.totalSize - compared});
            }
            return true;
        }

      public:
        GuestResourceRanges() = default;

        template<typename Mappings>
        explicit GuestResourceRanges(const Mappings &mappings) {
            for (const auto &mapping : mappings) {
                const auto address{reinterpret_cast<std::uintptr_t>(mapping.data())};
                if (!address || !mapping.size() ||
                    mapping.size() > std::numeric_limits<std::uintptr_t>::max() - address ||
                    mapping.size() > std::numeric_limits<std::size_t>::max() - totalSize) {
                    valid = false;
                    segments.clear();
                    totalSize = 0;
                    return;
                }
                segments.push_back({address, mapping.size(), totalSize});
                totalSize += mapping.size();
            }
            valid = valid && !segments.empty();
        }

        bool Valid() const { return valid && !segments.empty(); }
        std::size_t Size() const { return totalSize; }
        const std::vector<Segment> &Segments() const { return segments; }

        /** An address can occur twice at different logical offsets; ambiguous offsets are rejected. */
        std::optional<std::size_t> FindUniqueOffset(const std::uint8_t *address) const {
            const auto value{reinterpret_cast<std::uintptr_t>(address)};
            std::optional<std::size_t> result;
            for (const auto &segment : segments) {
                if (value < segment.address || value >= segment.End())
                    continue;
                if (result)
                    return std::nullopt;
                result = segment.logicalOffset + (value - segment.address);
            }
            return result;
        }

        /** Find a complete ordered physical sequence, regardless of how either side was split. */
        std::optional<std::size_t> FindContainedOffset(const GuestResourceRanges &other) const {
            if (!Valid() || !other.Valid() || other.totalSize > totalSize)
                return std::nullopt;

            std::optional<std::size_t> result;
            for (const auto &segment : segments) {
                const auto first{other.segments.front().address};
                if (first < segment.address || first >= segment.End())
                    continue;
                const auto offset{segment.logicalOffset + (first - segment.address)};
                if (offset > totalSize - other.totalSize || !MatchesAt(offset, other))
                    continue;
                if (result)
                    return std::nullopt;
                result = offset;
            }
            return result;
        }

        bool Overlaps(const GuestResourceRanges &other) const {
            for (const auto &lhs : segments)
                for (const auto &rhs : other.segments)
                    if (lhs.address < rhs.End() && rhs.address < lhs.End())
                        return true;
            return false;
        }

        /** Slices a logical region into physical pieces without assuming adjacent mappings. */
        std::vector<Segment> Slice(std::size_t offset, std::size_t size) const {
            std::vector<Segment> result;
            if (!Valid() || offset > totalSize || size > totalSize - offset)
                return result;
            for (const auto &segment : segments) {
                const auto begin{std::max(offset, segment.logicalOffset)};
                const auto end{std::min(offset + size, segment.logicalOffset + segment.size)};
                if (begin < end)
                    result.push_back({segment.address + begin - segment.logicalOffset, end - begin, begin});
            }
            return result;
        }
    };

    /** Group only when one whole guest resource is a physically ordered part of the other. */
    inline bool IsCompleteGuestAlias(const GuestResourceRanges &lhs, const GuestResourceRanges &rhs) {
        return lhs.FindContainedOffset(rhs).has_value() || rhs.FindContainedOffset(lhs).has_value();
    }

    struct SubresourceOffset {
        std::size_t layer{};
        std::size_t mip{};
        std::size_t offsetWithinMip{};
    };

    /** Locates a byte in a layered image; padding between layers is not a subresource. */
    inline std::optional<SubresourceOffset> LocateSubresource(std::size_t offset, std::size_t layerStride,
                                                               std::span<const std::size_t> mipSizes, std::size_t layerCount) {
        if (!layerStride || !layerCount || mipSizes.empty() || offset / layerStride >= layerCount)
            return std::nullopt;

        std::size_t occupied{};
        for (const auto size : mipSizes) {
            if (!size || size > layerStride - occupied)
                return std::nullopt;
            occupied += size;
        }

        auto withinLayer{offset % layerStride};
        for (std::size_t mip{}; mip < mipSizes.size(); ++mip) {
            if (withinLayer < mipSizes[mip])
                return SubresourceOffset{offset / layerStride, mip, withinLayer};
            withinLayer -= mipSizes[mip];
        }
        return std::nullopt;
    }
}
