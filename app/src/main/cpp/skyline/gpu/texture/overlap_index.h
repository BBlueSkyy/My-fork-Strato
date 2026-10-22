// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace skyline::gpu::texture {
    // Keep the original compact median traversal for cached mappings. A small
    // sorted tail absorbs new mappings without rebuilding that entire tree on
    // every texture creation. Both trees refer to the manager's existing vector.
    class MappingOverlapIndex {
      private:
        static constexpr size_t PendingLimit{128};
        std::vector<uintptr_t> maxEnds;
        std::vector<uintptr_t> pendingMaxEnds;
        std::vector<uintptr_t> prefixMaxEnds;
        size_t indexedCount{}, mappingCount{};
        uintptr_t widestStart{}, widestEnd{};

        template<typename Index>
        static uintptr_t Build(const Index &index, std::vector<uintptr_t> &ends, size_t first, size_t last) {
            if (first == last)
                return 0;
            const auto middle{first + (last - first) / 2};
            const auto &entry{index[middle]};
            const auto end{reinterpret_cast<uintptr_t>(entry.data()) + entry.size()};
            return ends[middle] = std::max({end, Build(index, ends, first, middle), Build(index, ends, middle + 1, last)});
        }

        template<typename Index, typename Visitor>
        static void Visit(const Index &index, const std::vector<uintptr_t> &ends, size_t first, size_t last,
                          uintptr_t begin, uintptr_t end, Visitor &visit) {
            if (first == last)
                return;
            const auto middle{first + (last - first) / 2};
            if (ends[middle] <= begin || reinterpret_cast<uintptr_t>(index[first].data()) >= end)
                return;
            Visit(index, ends, middle + 1, last, begin, end, visit);
            const auto &entry{index[middle]};
            const auto entryBegin{reinterpret_cast<uintptr_t>(entry.data())};
            if (entry.size() && entryBegin < end && entryBegin + entry.size() > begin)
                visit(entry);
            Visit(index, ends, first, middle, begin, end, visit);
        }

      public:
        template<typename Index>
        void Rebuild(const Index &index) {
            indexedCount = mappingCount = 0;
            widestStart = widestEnd = 0;
            Update(index, 0);
        }

        // Entries before firstChanged must be unchanged. The caller supplies
        // the earliest insertion position after inserting a sorted batch.
        template<typename Index>
        void Update(const Index &index, size_t firstChanged) {
            assert(firstChanged <= mappingCount && index.size() >= mappingCount);
            mappingCount = index.size();
            prefixMaxEnds.resize(mappingCount);
            auto prefixEnd{firstChanged ? prefixMaxEnds[firstChanged - 1] : uintptr_t{}};
            for (auto position{firstChanged}; position < mappingCount; ++position) {
                const auto &entry{index[position]};
                const auto begin{reinterpret_cast<uintptr_t>(entry.data())};
                prefixEnd = std::max(prefixEnd, begin + entry.size());
                prefixMaxEnds[position] = prefixEnd;
                if (entry.size() > widestEnd - widestStart) {
                    widestStart = begin;
                    widestEnd = begin + entry.size();
                }
            }
            // Insertion into the indexed prefix changes its positions. Appends
            // and insertions confined to the pending tail leave it intact.
            if (firstChanged < indexedCount || mappingCount - indexedCount > PendingLimit || !indexedCount) {
                indexedCount = mappingCount;
                maxEnds.resize(indexedCount);
                Build(index, maxEnds, 0, indexedCount);
                pendingMaxEnds.clear();
            } else {
                const auto pending{std::span{index}.subspan(indexedCount)};
                pendingMaxEnds.resize(pending.size());
                Build(pending, pendingMaxEnds, 0, pending.size());
            }
        }

        template<typename Range, typename Index, typename Visitor>
        void ForEachOverlap(const Range &range, const Index &index, Visitor &&visit) const {
            assert(mappingCount == index.size());
            if (!mappingCount || range.empty() || !range.data())
                return;
            const auto begin{reinterpret_cast<uintptr_t>(range.data())};
            const auto end{begin + range.size()};
            if (prefixMaxEnds.back() <= begin)
                return;
            // Most mappings do not overlap. A start search plus the preceding
            // prefix maximum proves whether only one candidate is possible.
            // A known containing allocation goes straight to the alias trees.
            if (begin < widestStart || end > widestEnd) {
                const auto last{std::lower_bound(index.begin(), index.end(), end,
                    [](const auto &entry, uintptr_t address) { return reinterpret_cast<uintptr_t>(entry.data()) < address; })};
                const auto candidateCount{static_cast<size_t>(last - index.begin())};
                if (!candidateCount)
                    return;
                if (candidateCount == 1 || prefixMaxEnds[candidateCount - 2] <= begin) {
                    const auto &entry{index[candidateCount - 1]};
                    if (entry.size() && reinterpret_cast<uintptr_t>(entry.data()) + entry.size() > begin)
                        visit(entry);
                    return;
                }
            }
            // Tail indices are greater than every indexed-prefix position;
            // visiting it first preserves the original descending vector order.
            const auto mappings{std::span{index}};
            const auto pending{mappings.subspan(indexedCount)};
            Visit(pending, pendingMaxEnds, 0, pending.size(), begin, end, visit);
            Visit(mappings, maxEnds, 0, indexedCount, begin, end, visit);
        }
    };
}
