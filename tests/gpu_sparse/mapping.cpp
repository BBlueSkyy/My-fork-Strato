#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <iostream>
#define TRACE_EVENT(...)
using u8 = uint8_t;
using VaType = uint64_t;
template<typename T> struct span {
    T *ptr{};
    size_t count{};
    span(T *ptr, size_t count) : ptr(ptr), count(count) {}
    T *data() const { return ptr; }
    size_t size() const { return count; }
    bool valid() const { return ptr != nullptr; }
};
using TranslatedAddressRange = std::vector<span<u8>>;
struct Memory {
    static constexpr VaType AddressSpaceSize{1ULL << 32};
    struct Block {
        VaType virt;
        u8 *phys;
        struct { bool sparseMapped; } extraInfo;
    };
    std::vector<Block> blocks;
    std::shared_mutex blockMutex;
    TranslatedAddressRange TranslateRangeImpl(VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback = {})
#include "translate.inc"
    bool IsFullySparseRange(VaType virt, VaType size)
#include "query.inc"
};
int main() {
    u8 data[0x2000]{};
    auto placeholder = reinterpret_cast<u8 *>(0xCAFEBABE);
    Memory m;
    m.blocks = {{0, nullptr, {}}, {0x1000, placeholder, {true}},
                {0x2000, placeholder, {true}}, {0x3000, data, {}}, {0x4000, nullptr, {}}};
    size_t callbackBytes{};
    const auto spans = m.TranslateRangeImpl(0x1800, 0x2000, [&](auto s) { callbackBytes += s.size(); });
    assert(spans.size() == 3);
    assert(!spans[0].valid() && spans[0].size() == 0x800);
    assert(!spans[1].valid() && spans[1].size() == 0x1000);
    assert(spans[2].data() == data && spans[2].size() == 0x800);
    assert(callbackBytes == 0x800); // sparse placeholders must never reach CPU callbacks
    assert(m.IsFullySparseRange(0x1800, 0x1800));
    assert(!m.IsFullySparseRange(0x1800, 0x1801)); // last byte resident
    assert(!m.IsFullySparseRange(0x800, 0x1000)); // first byte unmapped
    assert(!m.IsFullySparseRange(0x1000, 0));
    assert(!m.IsFullySparseRange(Memory::AddressSpaceSize, 1));
    assert(!m.IsFullySparseRange(0x1000, std::numeric_limits<VaType>::max()));
    m.blocks[2] = {0x2000, nullptr, {}};
    assert(!m.IsFullySparseRange(0x1800, 0x1000)); // sparse followed by a hole
    m.blocks[2] = {0x2000, data, {}};
    assert(!m.IsFullySparseRange(0x1800, 0x1000)); // physical remap supersedes sparse
    m.blocks = {{0, nullptr, {}}, {0x1000, data, {}}, {0x2000, data + 0x1000, {}}, {0x3000, nullptr, {}}};
    const auto resident = m.TranslateRangeImpl(0x1800, 0x1000);
    assert(resident.size() == 1 && resident[0].data() == data + 0x800 && resident[0].size() == 0x1000);
    std::cout << "PASS: sparse split translation, whole-range residency, remap, resident coalescing\n";
}
