// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace skyline::gpu::texture {
    // Augment TextureManager's existing start-sorted vector, rather than
    // introducing another texture owner. Each median stores the maximum end
    // in its subtree. Nested intervals therefore cannot hide earlier overlaps.
    // Rebuild once after insertion (already O(N) for the mapping vector);
    // queries allocate nothing and prune disjoint subtrees.
    class MappingOverlapIndex {
      private:
        std::vector<uintptr_t> maxEnds;

        template<typename Index>
        uintptr_t Build(const Index &index, size_t first, size_t last) {
            if (first == last)
                return 0;
            const auto middle{first + (last - first) / 2};
            const auto &entry{index[middle]};
            const auto end{reinterpret_cast<uintptr_t>(entry.data()) + entry.size()};
            return maxEnds[middle] = std::max({end, Build(index, first, middle), Build(index, middle + 1, last)});
        }

        template<typename Index, typename Visitor>
        void Visit(const Index &index, size_t first, size_t last, uintptr_t begin, uintptr_t end, Visitor &visit) const {
            if (first == last)
                return;
            const auto middle{first + (last - first) / 2};
            if (maxEnds[middle] <= begin || reinterpret_cast<uintptr_t>(index[first].data()) >= end)
                return;
            // Preserve the manager's descending start-address candidate order.
            Visit(index, middle + 1, last, begin, end, visit);
            const auto &entry{index[middle]};
            const auto entryBegin{reinterpret_cast<uintptr_t>(entry.data())};
            if (entry.size() && entryBegin < end && entryBegin + entry.size() > begin)
                visit(entry);
            Visit(index, first, middle, begin, end, visit);
        }

      public:
        template<typename Index>
        void Rebuild(const Index &index) {
            maxEnds.resize(index.size());
            Build(index, 0, index.size());
        }

        template<typename Range, typename Index, typename Visitor>
        void ForEachOverlap(const Range &range, const Index &index, Visitor &&visit) const {
            assert(maxEnds.size() == index.size());
            if (range.empty() || !range.data())
                return;
            const auto begin{reinterpret_cast<uintptr_t>(range.data())};
            Visit(index, 0, index.size(), begin, begin + range.size(), visit);
        }
    };
}
