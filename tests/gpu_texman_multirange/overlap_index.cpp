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

    // A new suffix mapping must not rescan every old entry. Measure address
    // reads rather than unstable wall time.
    const auto appendPosition{entries.size()};
    entries.push_back({{memory.data() + 20000, 2}, 5000});
    Entry::reads = 0;
    index.Update(entries, appendPosition);
    assert(Entry::reads < 100);
    found.clear();
    index.ForEachOverlap(std::span{memory.data() + 20000, 1}, entries, [&](const Entry &entry) { found.push_back(entry.id); });
    assert((found == std::vector<int>{5000, -1}));

    // Check insertion/rebuild, duplicate starts, nested and fragmented ranges
    // against an exhaustive oracle, including descending visitation order.
    std::mt19937 random{385};
    entries.clear();
    index.Rebuild(entries);
    for (int iteration{}; iteration < 80; ++iteration) {
        size_t start{random() % 30000};
        const Entry entry{{memory.data() + start, random() % 2000 + 1}, iteration};
        const auto insertion{std::upper_bound(entries.begin(), entries.end(), entry,
            [](const Entry &a, const Entry &b) { return a.range.data() < b.range.data(); })};
        const auto firstChanged{static_cast<size_t>(insertion - entries.begin())};
        entries.insert(insertion, entry);
        index.Update(entries, firstChanged);
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

    // Exercise several pending-tail flushes and cap the total index work.
    entries.clear();
    index.Rebuild(entries);
    size_t updateReads{};
    for (int i{}; i < 1024; ++i) {
        entries.push_back({{memory.data() + i * 16, 8}, i});
        Entry::reads = 0;
        index.Update(entries, entries.size() - 1);
        updateReads += Entry::reads;
        found.clear();
        index.ForEachOverlap(std::span{memory.data() + i * 16, 1}, entries,
            [&](const Entry &entry) { found.push_back(entry.id); });
        assert((found == std::vector<int>{i}));
    }
    assert(updateReads < 100000); // Rebuilding every prefix costs 524,800 reads.
    // Insert first in the pending tail, then in the indexed prefix. Both
    // overlapping results must remain in descending vector order.
    for (const auto &[position, id] : std::array{std::pair{950, 4000}, std::pair{0, 5000}}) {
        const Entry entry{{memory.data() + position * 16 + 4, 2}, id};
        const auto insertion{std::upper_bound(entries.begin(), entries.end(), entry,
            [](const Entry &a, const Entry &b) { return a.range.data() < b.range.data(); })};
        const auto firstChanged{static_cast<size_t>(insertion - entries.begin())};
        entries.insert(insertion, entry);
        index.Update(entries, firstChanged);
        found.clear();
        index.ForEachOverlap(entry.range, entries, [&](const Entry &candidate) { found.push_back(candidate.id); });
        assert((found == std::vector<int>{id, position}));
    }
}
