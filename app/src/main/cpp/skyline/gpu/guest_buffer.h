// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <boost/container/small_vector.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#include <common/span.h>

namespace skyline::gpu {
    /**
     * @brief Ordered CPU mappings which form one contiguous guest-GPU buffer.
     *
     * The mappings are contiguous in the GPU virtual address space, but do not
     * need to be contiguous in the CPU address space. Physically adjacent CPU
     * mappings are normalized into one entry.
     */
    struct GuestBuffer {
        using Mappings = boost::container::small_vector<span<u8>, 3>;

        Mappings mappings;

        GuestBuffer() = default;
        GuestBuffer(span<u8> mapping) : mappings{mapping} {}

        GuestBuffer(Mappings pMappings) : mappings{std::move(pMappings)} {
            Normalize();
        }

        void Normalize() {
            Mappings normalized;
            normalized.reserve(mappings.size());

            for (auto mapping : mappings) {
                if (!mapping.size()) {
                    normalized.emplace_back(mapping);
                    continue;
                }

                if (!normalized.empty() && normalized.back().data() && mapping.data() &&
                    normalized.back().data() + normalized.back().size() == mapping.data()) {
                    normalized.back() = span<u8>{normalized.back().data(), normalized.back().size() + mapping.size()};
                } else {
                    normalized.emplace_back(mapping);
                }
            }

            mappings = std::move(normalized);
        }

        bool valid() const {
            if (mappings.empty())
                return false;

            // A repeated/overlapping physical region would make a physical address ambiguous:
            // an existing BufferView could not be redirected to a unique logical offset.
            // Keep such alias layouts out of this representation rather than guessing.
            for (size_t i{}; i < mappings.size(); ++i) {
                const auto &mapping{mappings[i]};
                if (!mapping.data() || !mapping.size())
                    return false;

                const auto begin{reinterpret_cast<uintptr_t>(mapping.data())};
                if (mapping.size() > std::numeric_limits<uintptr_t>::max() - begin)
                    return false;
                const auto end{begin + mapping.size()};

                for (size_t j{}; j < i; ++j) {
                    const auto &previous{mappings[j]};
                    const auto previousBegin{reinterpret_cast<uintptr_t>(previous.data())};
                    const auto previousEnd{previousBegin + previous.size()};
                    if (begin < previousEnd && previousBegin < end)
                        return false;
                }
            }

            return true;
        }

        size_t size() const {
            size_t total{};
            for (const auto &mapping : mappings)
                total += mapping.size();
            return total;
        }

        std::optional<size_t> Find(span<u8> mapping) const {
            size_t offset{};
            for (const auto &region : mappings) {
                if (region.contains(mapping))
                    return offset + static_cast<size_t>(mapping.data() - region.data());
                offset += region.size();
            }
            return std::nullopt;
        }

        std::optional<size_t> Find(const GuestBuffer &mapping) const {
            if (!mapping.valid())
                return std::nullopt;

            std::optional<size_t> start;
            size_t consumed{};

            for (const auto &region : mapping.mappings) {
                auto offset{Find(region)};
                if (!offset)
                    return std::nullopt;

                if (!start)
                    start = *offset;
                else if (*offset != *start + consumed)
                    return std::nullopt;

                consumed += region.size();
            }

            return start;
        }

        /**
         * @brief Merges two partially-overlapping logical mapping sequences when their
         *        shared physical bytes have one consistent logical displacement.
         *
         * This allows rolling/windowed mappings such as [tail, head] and a shifted
         * [tail', larger-head] view to share one canonical buffer without making
         * existing BufferView offsets non-affine. Reordered aliases remain rejected.
         */
        std::optional<GuestBuffer> Merge(const GuestBuffer &other) const {
            if (!valid() || !other.valid())
                return std::nullopt;

            const auto thisSize{size()};
            const auto otherSize{other.size()};
            if (thisSize > static_cast<size_t>(std::numeric_limits<std::int64_t>::max()) ||
                otherSize > static_cast<size_t>(std::numeric_limits<std::int64_t>::max()))
                return std::nullopt;

            std::optional<std::int64_t> otherBase;
            size_t thisOffset{};

            for (const auto &lhs : mappings) {
                const auto lhsBegin{reinterpret_cast<uintptr_t>(lhs.data())};
                const auto lhsEnd{lhsBegin + lhs.size()};
                size_t otherOffset{};

                for (const auto &rhs : other.mappings) {
                    const auto rhsBegin{reinterpret_cast<uintptr_t>(rhs.data())};
                    const auto rhsEnd{rhsBegin + rhs.size()};
                    const auto overlapBegin{std::max(lhsBegin, rhsBegin)};
                    const auto overlapEnd{std::min(lhsEnd, rhsEnd)};

                    if (overlapBegin < overlapEnd) {
                        const auto lhsLogical{static_cast<std::int64_t>(thisOffset + (overlapBegin - lhsBegin))};
                        const auto rhsLogical{static_cast<std::int64_t>(otherOffset + (overlapBegin - rhsBegin))};
                        const auto candidate{lhsLogical - rhsLogical};

                        if (otherBase && *otherBase != candidate)
                            return std::nullopt;
                        otherBase = candidate;
                    }

                    otherOffset += rhs.size();
                }

                thisOffset += lhs.size();
            }

            if (!otherBase)
                return std::nullopt;

            if (*otherBase > 0 &&
                otherSize > static_cast<size_t>(std::numeric_limits<std::int64_t>::max() - *otherBase))
                return std::nullopt;

            const std::int64_t lhsBegin{};
            const auto lhsEnd{static_cast<std::int64_t>(thisSize)};
            const auto rhsBegin{*otherBase};
            const auto rhsEnd{rhsBegin + static_cast<std::int64_t>(otherSize)};
            const auto mergedBegin{std::min(lhsBegin, rhsBegin)};
            const auto mergedEnd{std::max(lhsEnd, rhsEnd)};

            struct Segment {
                std::int64_t begin;
                std::int64_t end;
                uintptr_t physical;
            };

            std::vector<Segment> segments;
            std::vector<std::int64_t> boundaries;
            segments.reserve(mappings.size() + other.mappings.size());
            boundaries.reserve((mappings.size() + other.mappings.size()) * 2);

            auto appendSegments{[&](const GuestBuffer &buffer, std::int64_t base) {
                std::int64_t logical{base};
                for (const auto &mapping : buffer.mappings) {
                    const auto mappingSize{static_cast<std::int64_t>(mapping.size())};
                    segments.push_back({logical, logical + mappingSize, reinterpret_cast<uintptr_t>(mapping.data())});
                    boundaries.push_back(logical);
                    logical += mappingSize;
                    boundaries.push_back(logical);
                }
            }};

            appendSegments(*this, 0);
            appendSegments(other, *otherBase);

            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

            Mappings mergedMappings;
            for (size_t i{}; i + 1 < boundaries.size(); ++i) {
                const auto begin{boundaries[i]};
                const auto end{boundaries[i + 1]};
                if (begin < mergedBegin || end > mergedEnd || begin == end)
                    continue;

                std::optional<uintptr_t> physical;
                for (const auto &segment : segments) {
                    if (begin < segment.begin || end > segment.end)
                        continue;

                    const auto candidate{segment.physical + static_cast<uintptr_t>(begin - segment.begin)};
                    if (physical && *physical != candidate)
                        return std::nullopt;
                    physical = candidate;
                }

                if (!physical)
                    return std::nullopt;

                const auto length{static_cast<size_t>(end - begin)};
                if (length > std::numeric_limits<uintptr_t>::max() - *physical)
                    return std::nullopt;

                mergedMappings.emplace_back(reinterpret_cast<u8 *>(*physical), length);
            }

            GuestBuffer merged{std::move(mergedMappings)};
            if (!merged.valid() || merged.size() != static_cast<size_t>(mergedEnd - mergedBegin))
                return std::nullopt;

            return merged;
        }
    };
}
