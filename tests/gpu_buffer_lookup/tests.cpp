// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>
using u8 = uint8_t;
using ContextTag = uint64_t;

struct Iterator {
    u8 *pointer;
    u8 *base() const { return pointer; }
    auto operator<=>(const Iterator &) const = default;
};
template<typename T> struct span {
    T *first, *last;
    Iterator begin() const { return {first}; }
    Iterator end() const { return {last}; }
};
static uint64_t endReads{};
struct GuestRange : span<u8> {
    Iterator end() const { ++endReads; return {last}; }
    bool contains(span<u8> range) const { return first <= range.first && last >= range.last; }
};
struct Buffer : std::enable_shared_from_this<Buffer> { std::optional<GuestRange> guest; };
struct BufferManager {
    struct LockedBuffer {
        std::shared_ptr<Buffer> buffer;
        ContextTag tag;
        LockedBuffer(std::shared_ptr<Buffer> value, ContextTag context) : buffer{std::move(value)}, tag{context} {}
    };
    using LockedBuffers = std::vector<LockedBuffer>;
    std::vector<std::shared_ptr<Buffer>> bufferMappings;
    struct Table {
        Buffer *hit{};
        Buffer *operator[](u8 *) const { return hit; }
    } bufferTable;
    static bool BufferLessThan(const std::shared_ptr<Buffer> &, u8 *);
    LockedBuffers Lookup(span<u8>, ContextTag);
};
#include "production.inc"

static void Check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static void Add(BufferManager &manager, u8 *first, u8 *last) {
    if (!manager.bufferMappings.empty())
        Check(manager.bufferMappings.back()->guest->last <= first, "fixture violated disjoint-map invariant");
    auto buffer{std::make_shared<Buffer>()};
    buffer->guest = GuestRange{{first, last}};
    manager.bufferMappings.push_back(std::move(buffer));
}

static size_t cases{};
static void CheckLookup(BufferManager &manager, span<u8> range) {
    auto actual{manager.Lookup(range, 19)};
    std::vector<std::shared_ptr<Buffer>> expected;
    for (auto it{manager.bufferMappings.rbegin()}; it != manager.bufferMappings.rend(); ++it)
        if ((*it)->guest->first < range.last && (*it)->guest->last > range.first)
            expected.push_back(*it);
    Check(actual.size() == expected.size(), "overlap count differs from full-scan oracle");
    for (size_t i{}; i < actual.size(); ++i)
        Check(actual[i].buffer == expected[i] && actual[i].tag == 19, "overlap identity/context/order changed");
    ++cases;
}

static void Regression() {
    std::vector<u8> storage(8192 * 128);
    auto base{storage.data()};
    BufferManager manager;
    CheckLookup(manager, {base, base + 1});
    for (size_t i{}; i < 4096; ++i) Add(manager, base + i * 128, base + i * 128 + 64);
    // Covers exact edges, gaps, adjacent queries and several simultaneous overlaps.
    for (size_t first{}; first < 256; ++first)
        for (size_t last{first + 1}; last <= 256; ++last)
            CheckLookup(manager, {base + first, base + last});
    std::mt19937 random(42);
    for (size_t i{}; i < 10000; ++i) {
        size_t first{random() % (4096 * 128)}, last{random() % (4096 * 128)};
        if (first > last) std::swap(first, last);
        if (first == last) ++last;
        CheckLookup(manager, {base + first, base + last});
    }
    // Vary widths/gaps and include touching intervals, not only a uniform map.
    BufferManager irregular;
    size_t cursor{};
    for (size_t i{}; i < 512; ++i) {
        cursor += random() % 17;
        const auto length{1 + random() % 127};
        Add(irregular, base + cursor, base + cursor + length);
        cursor += length;
    }
    for (size_t i{}; i < 5000; ++i) {
        size_t first{random() % cursor}, last{random() % cursor};
        if (first > last) std::swap(first, last);
        if (first == last) ++last;
        CheckLookup(irregular, {base + first, base + last});
    }
    manager.bufferTable.hit = manager.bufferMappings.back().get();
    endReads = 0;
    CheckLookup(manager, {base + 4095 * 128, base + 4095 * 128 + 1});
    Check(endReads == 0, "page-table fast hit entered mapping scan");
    // A page-table candidate too small for the query must still use fallback.
    CheckLookup(manager, {base + 4094 * 128, base + 4095 * 128 + 1});
    manager.bufferTable.hit = nullptr;
    endReads = 0;
    CheckLookup(manager, {base + 4095 * 128, base + 4095 * 128 + 1});
    const auto tailReads{endReads};
    endReads = 0;
    CheckLookup(manager, {base + 4096 * 128, base + 4096 * 128 + 1});
    const auto gapReads{endReads};
    std::printf("oracle cases=%zu tail_end_reads=%llu gap_end_reads=%llu\n", cases,
                static_cast<unsigned long long>(tailReads), static_cast<unsigned long long>(gapReads));
    Check(tailReads <= 2 && gapReads <= 1, "fallback scanned irrelevant preceding disjoint buffers");
}

static void Benchmark() {
    std::vector<u8> storage(4097 * 128);
    auto base{storage.data()};
    BufferManager manager;
    for (size_t i{}; i < 4096; ++i) Add(manager, base + i * 128, base + i * 128 + 64);
    constexpr size_t iterations{30000};
    for (size_t sample{}; sample < 5; ++sample) {
        uint64_t checksum{};
        endReads = 0;
        auto start{std::chrono::steady_clock::now()};
        for (size_t i{}; i < iterations; ++i) {
            const auto offset{(4095 - i % 8) * 128};
            checksum += manager.Lookup({base + offset, base + offset + 1}, 19).size();
        }
        auto ms{std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()};
        Check(checksum == iterations, "benchmark changed overlap results");
        std::printf("synthetic sample=%zu queries=%zu end_reads=%llu ms=%.3f checksum=%llu\n", sample, iterations,
                    static_cast<unsigned long long>(endReads), ms, static_cast<unsigned long long>(checksum));
    }
}
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--benchmark") Benchmark();
    else Regression();
}
