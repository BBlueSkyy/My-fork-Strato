// SPDX-License-Identifier: MPL-2.0
// Standalone: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined
// tests/gpu_texman_multirange/test.cpp -o /tmp/texman-multirange && /tmp/texman-multirange
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>
#include "../../app/src/main/cpp/skyline/gpu/texture/secondary_overlap.h"

struct Entry {
    std::span<std::uint8_t> range;
    int id;
    auto begin() const { return range.begin(); }
    auto end() const { return range.end(); }
    auto data() const { return range.data(); }
    auto size() const { return range.size(); }
};

int main() {
    std::array<std::uint8_t, 1024> memory{};
    std::vector<Entry> entries{
        {{memory.data() + 600, 10}, 6},
        {{memory.data() + 410, 10}, 4}, // Only first mapping intersects
        {{memory.data() + 56, 8}, 9},   // Touches the secondary end
        {{memory.data() + 44, 28}, 2},  // Contains the secondary mapping
        {{memory.data() + 50, 4}, 1},   // Partially intersects the secondary
        {{memory.data() + 38, 10}, 3},  // Touches the secondary start
        {{memory.data() + 10, 9}, 0},
    };
    // Texman's mapping index uses start-address ordering, not end ordering.
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.begin() < b.begin();
    });
    std::vector<std::span<std::uint8_t>> guest{
        {memory.data() + 400, 12},
        {memory.data() + 48, 8},
    };

    std::vector<int> found;
    skyline::gpu::texture::MappingOverlapIndex overlaps;
    overlaps.Rebuild(entries);
    skyline::gpu::texture::ForEachSecondaryMappingOverlap(guest, entries, overlaps,
        [&](const Entry &entry) { found.push_back(entry.id); });
    std::sort(found.begin(), found.end());
    assert((found == std::vector<int>{1, 2}));

    guest.resize(1);
    found.clear();
    skyline::gpu::texture::ForEachSecondaryMappingOverlap(guest, entries, overlaps,
        [&](const Entry &entry) { found.push_back(entry.id); });
    assert(found.empty());

    guest.emplace_back(memory.data() + 48, 8);
    guest.emplace_back(memory.data() + 700, 8);
    found.clear();
    skyline::gpu::texture::ForEachSecondaryMappingOverlap(guest, entries, overlaps,
        [&](const Entry &entry) { found.push_back(entry.id); });
    std::sort(found.begin(), found.end());
    assert((found == std::vector<int>{1, 2}));
}
