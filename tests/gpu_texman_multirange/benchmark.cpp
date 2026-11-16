// SPDX-License-Identifier: MPL-2.0
// Optional host microbenchmark, excluded from run.py and APK workflows.
// c++ -std=c++20 -O3 -DNDEBUG tests/gpu_texman_multirange/benchmark.cpp -o /tmp/texman-bench
// /tmp/texman-bench [nested]
// To compare an older header: -DTEXMAN_INDEX_HEADER='"/tmp/old-overlap-index.h"'
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>
#ifndef TEXMAN_INDEX_HEADER
#define TEXMAN_INDEX_HEADER "../../app/src/main/cpp/skyline/gpu/texture/overlap_index.h"
#endif
#include TEXMAN_INDEX_HEADER

struct Entry {
    std::span<std::uint8_t> range;
    static inline std::uint64_t reads{};
    auto data() const { ++reads; return range.data(); }
    auto size() const { return range.size(); }
};

template<typename Index>
void Append(Index &index, const std::vector<Entry> &entries, size_t position) {
    if constexpr (requires { index.Update(entries, position); })
        index.Update(entries, position);
    else
        index.Rebuild(entries);
}

int main(int argc, char **) {
    constexpr size_t Count{12000}, Queries{240000};
    std::vector<std::uint8_t> memory(Count * 16);
    std::vector<Entry> entries;
    entries.reserve(Count);
    skyline::gpu::texture::MappingOverlapIndex index;
    auto begin{std::chrono::steady_clock::now()};
    for (size_t i{}; i < Count; ++i) {
        entries.push_back({{memory.data() + i * 16, argc > 1 && i == 0 ? memory.size() : size_t{8}}});
        Append(index, entries, i);
    }
    auto end{std::chrono::steady_clock::now()};
    std::cout << "append_ms=" << std::chrono::duration<double, std::milli>(end - begin).count()
              << " address_reads=" << Entry::reads << '\n';
    Entry::reads = 0;
    size_t hits{};
    begin = std::chrono::steady_clock::now();
    for (size_t i{}; i < Queries; ++i) {
        const std::span range{memory.data() + ((i * 7919) % Count) * 16, size_t{1}};
        index.ForEachOverlap(range, entries, [&](const Entry &) { ++hits; });
    }
    end = std::chrono::steady_clock::now();
    std::cout << "query_ms=" << std::chrono::duration<double, std::milli>(end - begin).count()
              << " address_reads=" << Entry::reads << " hits=" << hits << '\n';
    return hits == (argc > 1 ? Queries * 2 - Queries / Count : Queries) ? 0 : 1;
}
