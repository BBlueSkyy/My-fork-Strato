// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <span>
#include <vector>
#include "../../app/src/main/cpp/skyline/gpu/texture/overlap_index.h"

struct Entry {
    std::span<std::uint8_t> range;
    int id;
    static inline size_t reads{};
    auto data() const { ++reads; return range.data(); }
    auto size() const { return range.size(); }
};

int main() {
    using skyline::gpu::texture::MappingOverlapIndex;
    std::array<std::uint8_t, 32768> memory{};
    std::vector<Entry> entries;
    MappingOverlapIndex index;
    index.Rebuild(entries);
    index.ForEachOverlap(std::span{memory}, entries, [](const Entry &) { assert(false); });
    // Long enclosing interval followed by thousands of disjoint short intervals.
    entries.push_back({memory, -1});
    for (int i{}; i < 4096; ++i)
        entries.push_back({{memory.data() + 4 * i, 2}, i});
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.range.data() < b.range.data(); });
    index.Rebuild(entries);
    Entry::reads = 0;
    std::vector<int> found;
    index.ForEachOverlap(std::span{memory.data() + 16001, 1}, entries, [&](const Entry &entry) { found.push_back(entry.id); });
    assert((found == std::vector<int>{4000, -1}));
    assert(Entry::reads < 150); // Prune subtrees; a prefix-max linear walk fails.
    found.clear();
    index.ForEachOverlap(std::span{memory.data() + 16002, 2}, entries, [&](const Entry &entry) { found.push_back(entry.id); });
    assert((found == std::vector<int>{-1})); // Half-open boundary touches.
    index.ForEachOverlap(std::span<std::uint8_t>{}, entries, [](const Entry &) { assert(false); });

    // Check insertion/rebuild, duplicate starts, nested and fragmented ranges
    // against an exhaustive oracle, including descending visitation order.
    std::mt19937 random{385};
    entries.clear();
    for (int iteration{}; iteration < 80; ++iteration) {
        size_t start{random() % 30000};
        entries.push_back({{memory.data() + start, random() % 2000 + 1}, iteration});
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.range.data() < b.range.data(); });
        index.Rebuild(entries);
        for (int query{}; query < 20; ++query) {
            start = random() % 30000;
            const std::span range{memory.data() + start, random() % 2000 + 1};
            std::vector<int> expected;
            for (auto it{entries.rbegin()}; it != entries.rend(); ++it)
                if (it->range.data() < range.data() + range.size() && it->range.data() + it->range.size() > range.data())
                    expected.push_back(it->id);
            found.clear();
            index.ForEachOverlap(range, entries, [&](const Entry &entry) { found.push_back(entry.id); });
            assert(found == expected);
        }
    }
}
