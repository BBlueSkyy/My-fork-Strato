// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace skyline::gpu {
    /**
     * @brief GPFIFO-thread-owned physical memory hazards. Submission retires
     * sequenced writes; only a completed executor wait retires GPU hazards.
     * These are access hazards, not proof that guest bytes are authoritative.
     */
    class UsageTracker {
        class RangeSet {
            struct Interval {
                uintptr_t begin, end; // Half-open byte addresses, never pointer differences.
            };
            std::vector<Interval> intervals;

          public:
            template<typename Range>
            void Insert(const Range &range) {
                if (!range.data() || !range.size()) return;
                const auto begin{reinterpret_cast<uintptr_t>(range.data())};
                const auto end{begin + range.size()};
                auto first{std::lower_bound(intervals.begin(), intervals.end(), begin,
                    [](const Interval &entry, uintptr_t address) { return entry.end < address; })};
                if (first == intervals.end() || first->begin > end) {
                    intervals.insert(first, {begin, end});
                    return;
                }
                auto last{first + 1};
                auto mergedEnd{std::max(first->end, end)};
                while (last != intervals.end() && last->begin <= mergedEnd) {
                    mergedEnd = std::max(mergedEnd, last->end);
                    ++last;
                }
                first->begin = std::min(first->begin, begin);
                first->end = mergedEnd;
                intervals.erase(first + 1, last);
            }

            template<typename Range>
            bool Intersects(const Range &range) const {
                if (!range.data() || !range.size()) return false;
                const auto begin{reinterpret_cast<uintptr_t>(range.data())};
                const auto end{begin + range.size()};
                const auto it{std::lower_bound(intervals.begin(), intervals.end(), begin,
                    [](const Interval &entry, uintptr_t address) { return entry.end <= address; })};
                return it != intervals.end() && it->begin < end;
            }

            void Clear() { intervals.clear(); } // Retain capacity across submissions.
        };

        RangeSet pendingGpuWrites;
        RangeSet sequencedWrites;

      public:
        template<typename Range>
        void RecordGpuWrite(const Range &range) {
            pendingGpuWrites.Insert(range);
            sequencedWrites.Insert(range);
        }

        template<typename Range>
        void RecordSequencedWrite(const Range &range) { sequencedWrites.Insert(range); }

        template<typename Range>
        bool RequiresGpuFlush(const Range &range) const { return pendingGpuWrites.Intersects(range); }

        template<typename Range>
        bool HasSequencedWrite(const Range &range) const { return sequencedWrites.Intersects(range); }

        void OnSubmit() { sequencedWrites.Clear(); }
        void OnWaitComplete() { pendingGpuWrites.Clear(); }
    };
}
