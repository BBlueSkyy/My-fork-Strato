// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <boost/container/small_vector.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
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
    };
}
