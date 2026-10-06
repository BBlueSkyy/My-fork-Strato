// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <boost/container/small_vector.hpp>
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

            for (const auto &mapping : mappings)
                if (!mapping.data() || !mapping.size())
                    return false;

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
    };
}
