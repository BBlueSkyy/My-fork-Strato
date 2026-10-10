// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#include <common/span.h>

namespace skyline::gpu {
    /**
     * @brief Tracks guest-memory ranges whose ordering is relevant to GPU execution
     *
     * This tracker owns guest-memory visibility/sequencing only. Vulkan image hazards,
     * layouts and Texman representation authority are intentionally handled elsewhere.
     */
    class UsageTracker {
      private:
        using Address = std::uintptr_t;

        struct AddressRange {
            Address begin{};
            Address end{};

            constexpr bool Empty() const {
                return begin >= end;
            }
        };

        /**
         * @brief Union of half-open guest address intervals [begin, end)
         *
         * Integer addresses are used deliberately: comparing/subtracting raw pointers from
         * unrelated guest mappings is not a valid basis for an address interval structure.
         */
        class AddressRangeSet {
          private:
            std::vector<AddressRange> ranges;

          public:
            void Clear() {
                ranges.clear();
            }

            void Insert(AddressRange incoming) {
                if (incoming.Empty())
                    return;

                auto first{std::lower_bound(
                    ranges.begin(), ranges.end(), incoming.begin,
                    [](const AddressRange &range, Address begin) {
                        return range.end < begin;
                    })};

                while (first != ranges.end() && first->begin <= incoming.end) {
                    incoming.begin = std::min(incoming.begin, first->begin);
                    incoming.end = std::max(incoming.end, first->end);
                    first = ranges.erase(first);
                }

                ranges.insert(first, incoming);
            }

            bool Intersects(AddressRange query) const {
                if (query.Empty())
                    return false;

                const auto first{std::lower_bound(
                    ranges.begin(), ranges.end(), query.begin,
                    [](const AddressRange &range, Address begin) {
                        return range.end <= begin;
                    })};

                return first != ranges.end() && first->begin < query.end;
            }
        };

        AddressRangeSet gpuDirtyRanges;
        AddressRangeSet sequencedWriteRanges;

        static AddressRange ResolveRange(span<u8> range) {
            if (!range.valid() || range.empty())
                return {};

            const auto begin{reinterpret_cast<Address>(range.data())};
            const auto size{static_cast<Address>(range.size_bytes())};
            if (size > std::numeric_limits<Address>::max() - begin)
                return {begin, std::numeric_limits<Address>::max()};
            return {begin, begin + size};
        }

      public:
        void MarkGpuDirty(span<u8> range) {
            gpuDirtyRanges.Insert(ResolveRange(range));
        }

        bool IntersectsGpuDirty(span<u8> range) const {
            return gpuDirtyRanges.Intersects(ResolveRange(range));
        }

        void MarkSequencedWrite(span<u8> range) {
            sequencedWriteRanges.Insert(ResolveRange(range));
        }

        bool IntersectsSequencedWrite(span<u8> range) const {
            return sequencedWriteRanges.Intersects(ResolveRange(range));
        }

        void ResetSequencedWrites() {
            sequencedWriteRanges.Clear();
        }

        void ResetGpuDirty() {
            gpuDirtyRanges.Clear();
        }
    };
}
