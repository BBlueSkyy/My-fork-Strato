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
struct BlockInfo { bool sparseMapped{}; };
struct Memory {
    static constexpr VaType AddressSpaceSize{1ULL << 32};
    struct Block {
        VaType virt;
        u8 *phys;
        BlockInfo extraInfo;
        bool operator<(VaType other) const { return virt < other; }
    };
    struct SegmentEntry {
        VaType virt{};
        u8 *phys{};
        VaType extent{};
        BlockInfo extraInfo{};
    };
    struct SegmentTable {
        std::vector<SegmentEntry> pages{16};
        void Set(VaType start, VaType end, SegmentEntry entry) {
            assert(start < end && start % 0x1000 == 0 && end % 0x1000 == 0);
            for (VaType p = start; p < end; p += 0x1000)
                pages.at(p / 0x1000) = entry;
        }
        const SegmentEntry &operator[](VaType virt) const { return pages.at(virt / 0x1000); }
    } blockSegmentTable;
    std::vector<Block> blocks;
    std::shared_mutex blockMutex;
    void ClipPrecedingSegmentExtent(VaType virt)
#include "clip.inc"
    std::pair<span<u8>, size_t> LookupBlockLocked(VaType virt, std::function<void(span<u8>)> cpuAccessCallback = {})
#include "lookup.inc"
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
    // Partial remap: the segment-table's preceding entry used to retain its
    // full original extent and swallow the newly resident tail.
    m.blocks = {{0, nullptr, {}}, {0x1000, placeholder, {true}},
                {0x3000, data, {}}, {0x4000, nullptr, {}}};
    m.blockSegmentTable.Set(0x1000, 0x4000, {0x1000, placeholder, 0x3000, {true}});
    m.ClipPrecedingSegmentExtent(0x3000);
    m.blockSegmentTable.Set(0x3000, 0x4000, {0x3000, data, 0x1000, {}});
    assert(m.blockSegmentTable[0x1000].extent == 0x2000);
    const auto sparseThenResident = m.TranslateRange(0x1800, 0x2000);
    assert(sparseThenResident.size() == 2);
    assert(!sparseThenResident[0].valid() && sparseThenResident[0].size() == 0x1800);
    assert(sparseThenResident[1].valid() && sparseThenResident[1].size() == 0x800);

    // Resident -> sparse -> resident: a mapping must not be allowed to skip an
    // internal sparse remap even if both ends have the same physical backing.
    m.blocks = {{0, nullptr, {}}, {0x1000, data, {}},
                {0x2000, placeholder, {true}}, {0x3000, data + 0x2000, {}},
                {0x4000, nullptr, {}}};
    m.blockSegmentTable.Set(0x1000, 0x4000, {0x1000, data, 0x3000, {}});
    m.ClipPrecedingSegmentExtent(0x2000);
    m.blockSegmentTable.Set(0x2000, 0x3000, {0x2000, placeholder, 0x1000, {true}});
    assert(m.blockSegmentTable[0x1000].extent == 0x1000);
    size_t translatedResidentBytes{};
    const auto residentThenSparse = m.TranslateRange(0x1800, 0x2000, [&](auto s) {
        translatedResidentBytes += s.size();
    });
    assert(residentThenSparse.size() == 3);
    assert(residentThenSparse[0].valid() && !residentThenSparse[1].valid() && residentThenSparse[2].valid());
    assert(translatedResidentBytes == 0x1000);

    // Regular resident lookup remains a single fast table lookup with no
    // block-vector search; callback scope remains exactly the requested range.
    m.blocks = {{0, nullptr, {}}, {0x1000, data, {}}, {0x4000, nullptr, {}}};
    m.blockSegmentTable.Set(0x1000, 0x4000, {0x1000, data, 0x3000, {}});
    size_t fastCallbackBytes{};
    const auto mappedFast = m.TranslateRange(0x1800, 0x800, [&](auto s) { fastCallbackBytes += s.size(); });
    assert(mappedFast.size() == 1 && mappedFast[0].valid() && fastCallbackBytes == 0x800);

    // A later unmap also clips an earlier resident span: the original mapping
    // must not remain visible across the newly unmapped boundary.
    m.blocks = {{0, nullptr, {}}, {0x1000, data, {}}, {0x2000, nullptr, {}},
                {0x4000, nullptr, {}}};
    m.ClipPrecedingSegmentExtent(0x2000);
    m.blockSegmentTable.Set(0x2000, 0x3000, {});
    const auto mappedThenHole = m.TranslateRange(0x1800, 0x1000);
    assert(mappedThenHole.size() == 2);
    assert(mappedThenHole[0].valid() && !mappedThenHole[1].valid());

    std::cout << "PASS: sparse split translation, clip-on-remap, resident fast path and callbacks\\n";
}
