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
    bool empty() const { return count == 0; }
    span subspan(size_t offset, size_t size) const {
        assert(offset <= count && size <= count - offset);
        return {ptr ? ptr + offset : nullptr, size};
    }
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
    // Simulates a segment-table entry whose original extent survived a partial remap.
    span<u8> staleFastSpan{nullptr, 0};
    VaType staleFastStart{};
    std::pair<span<u8>, size_t> LookupBlockLocked(VaType virt, std::function<void(span<u8>)> callback = {}) {
        assert(staleFastSpan.size() && virt >= staleFastStart);
        if (callback && staleFastSpan.valid())
            callback(staleFastSpan);
        return {staleFastSpan, static_cast<size_t>(virt - staleFastStart)};
    }
    TranslatedAddressRange TranslateRange(VaType virt, VaType size, std::function<void(span<u8>)> cpuAccessCallback = {})
#include "fastpath.inc"
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
    // A sparse mapping partially remapped to resident memory must not be
    // swallowed by the original broad null fast-path entry.
    m.blocks = {{0, nullptr, {}}, {0x1000, placeholder, {true}},
                {0x2000, placeholder, {true}}, {0x3000, data, {}}, {0x4000, nullptr, {}}};
    m.staleFastSpan = {nullptr, 0x3000};
    m.staleFastStart = 0x1000;
    const auto sparseThenResident = m.TranslateRange(0x1800, 0x2000);
    assert(sparseThenResident.size() == 3);
    assert(!sparseThenResident[0].valid() && !sparseThenResident[1].valid());
    assert(sparseThenResident[2].valid() && sparseThenResident[2].size() == 0x800);

    // Even if both ends of an originally resident span are still resident,
    // an internal sparse remap must be respected. No callback on ghost memory.
    m.blocks = {{0, nullptr, {}}, {0x1000, data, {}},
                {0x2000, placeholder, {true}}, {0x3000, data + 0x2000, {}}, {0x4000, nullptr, {}}};
    m.staleFastSpan = {data, 0x3000};
    size_t translatedResidentBytes{};
    const auto residentThenSparse = m.TranslateRange(0x1800, 0x2000, [&](auto s) {
        translatedResidentBytes += s.size();
    });
    assert(residentThenSparse.size() == 3);
    assert(residentThenSparse[0].valid() && !residentThenSparse[1].valid() && residentThenSparse[2].valid());
    assert(translatedResidentBytes == 0x1000);

    // The true single-block fast path remains functional.
    m.blocks = {{0, nullptr, {}}, {0x1000, data, {}}, {0x4000, nullptr, {}}};
    size_t fastCallbackBytes{};
    const auto mappedFast = m.TranslateRange(0x1800, 0x800, [&](auto s) { fastCallbackBytes += s.size(); });
    assert(mappedFast.size() == 1 && mappedFast[0].valid() && fastCallbackBytes == 0x800);
    std::cout << "PASS: sparse split translation, remap-aware fast path, callback extents\n";
}
