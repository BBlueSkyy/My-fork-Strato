// SPDX-License-Identifier: MPL-2.0
// Exact production nso.cpp/zbic.cpp and Backing; only Android/kernel load target is shimmed.
#include <functional>
#include <iostream>
#include <sys/resource.h>
#include <lz4.h>
#include <zstd.h>
#include <kernel/types/KProcess.h>
#include "nso.h"
using namespace skyline;
using namespace skyline::loader;
namespace {
void Require(bool passed, const char *message) { if (!passed) throw std::runtime_error(message); }
class Bytes : public vfs::Backing {
public:
    std::vector<u8> bytes;
    bool shortRead{};
    explicit Bytes(std::vector<u8> bytes) : Backing({true, false, false}, bytes.size()), bytes(std::move(bytes)) {}
    size_t ReadImpl(span<u8> out, size_t offset) override {
        const size_t count{shortRead && out.size() ? out.size() - 1 : out.size()};
        Require(offset <= bytes.size() && count <= bytes.size() - offset, "unexpected backing read");
        if (count) std::memcpy(out.data(), bytes.data() + offset, count);
        return count;
    }
};
void Write32(std::vector<u8> &bytes, size_t offset, u32 value) { std::memcpy(bytes.data() + offset, &value, 4); }
std::shared_ptr<Bytes> MakeNso(std::vector<u8> segment, u32 outputSize, bool compressed = false, bool zbic = false) {
    std::vector<u8> bytes(0x100 + segment.size());
    Write32(bytes, 0, util::MakeMagic<u32>("NSO0"));
    Write32(bytes, 0xC, (compressed ? 1 : 0) | (zbic ? 0x80 : 0));
    Write32(bytes, 0x10, 0x100);
    Write32(bytes, 0x18, outputSize);
    Write32(bytes, 0x60, segment.size());
    std::copy(segment.begin(), segment.end(), bytes.begin() + 0x100);
    return std::make_shared<Bytes>(std::move(bytes));
}
void Load(const std::shared_ptr<Bytes> &file) {
    capturedExecutable.reset();
    auto process{std::make_shared<kernel::type::KProcess>()};
    DeviceState state{};
    state.process = process;
    NsoLoader loader(file);
    NsoLoader::LoadNso(&loader, file, process, state);
}
void Rejects(const std::function<void()> &run, std::string_view expected) {
    try { run(); } catch (const skyline::exception &error) {
        Require(std::string_view(error.what()).find(expected) != std::string_view::npos, error.what());
        Require(!capturedExecutable, "invalid segment reached LoadExecutable");
        return;
    }
    throw std::runtime_error("malformed NSO accepted");
}
void Raw() {
    const std::vector<u8> expected{1, 2, 3, 4, 5, 6, 7, 8};
    Load(MakeNso(expected, expected.size()));
    Require(std::equal(expected.begin(), expected.end(), capturedExecutable->text.contents.begin()), "raw segment changed");
}
void Lz4() {
    const std::string expected{"Valid NSO LZ4 segment with repeated repeated repeated data"};
    std::vector<u8> compressed(LZ4_compressBound(expected.size()));
    const auto size{LZ4_compress_default(expected.data(), reinterpret_cast<char *>(compressed.data()), expected.size(), compressed.size())};
    Require(size > 0, "fixture compression failed");
    compressed.resize(size);
    Load(MakeNso(compressed, expected.size(), true));
    Require(std::equal(expected.begin(), expected.end(), capturedExecutable->text.contents.begin()), "LZ4 segment changed");
}
void Zbic() {
    const std::string expected{"Valid ZBIC NSO"};
    // Zstandard single-segment frame with one raw block, using ZBIC's magic.
    // This preserves routing coverage without pretending standard FSE tables are BIC.
    const u32 blockHeader{static_cast<u32>((expected.size() << 3) | 1)};
    std::vector<u8> compressed{0x5A, 0x42, 0x49, 0x43, 0x20, static_cast<u8>(expected.size()),
        static_cast<u8>(blockHeader), static_cast<u8>(blockHeader >> 8), static_cast<u8>(blockHeader >> 16)};
    compressed.insert(compressed.end(), expected.begin(), expected.end());
    Load(MakeNso(compressed, expected.size(), true, true));
    Require(std::equal(expected.begin(), expected.end(), capturedExecutable->text.contents.begin()), "ZBIC segment changed");
}
void InvalidLz4() { Rejects([] { Load(MakeNso({0xFF}, 8, true)); }, "LZ4"); }
void ShortLz4() { Rejects([] { Load(MakeNso({0x10, 'a'}, 8, true)); }, "LZ4"); }
void ZeroCompressedSize() {
    auto file{MakeNso({'r', 'a', 'w', '!'}, 4, true)};
    Write32(file->bytes, 0x60, 0);
    Rejects([&] { Load(file); }, "compressed size");
}
void ShortRead() {
    auto file{MakeNso({1, 2, 3, 4}, 4)};
    // Constructor and header succeed; only the segment read is short.
    class ShortSegment : public Bytes {
    public:
        using Bytes::Bytes;
        size_t ReadImpl(span<u8> out, size_t offset) override {
            shortRead = offset == 0x100;
            return Bytes::ReadImpl(out, offset);
        }
    };
    Rejects([&] { Load(std::make_shared<ShortSegment>(file->bytes)); }, "short read");
}
void HeaderShortRead() {
    auto file{MakeNso({}, 0)};
    class ShortHeader : public Bytes {
    public:
        using Bytes::Bytes;
        size_t ReadImpl(span<u8> out, size_t offset) override {
            shortRead = offset == 0 && out.size() == 0x100;
            return Bytes::ReadImpl(out, offset);
        }
    };
    Rejects([&] { Load(std::make_shared<ShortHeader>(file->bytes)); }, "header short read");
}
void RangeBeforeAllocation() {
    auto file{MakeNso({}, 0x80000000)};
    Rejects([&] { Load(file); }, "range");
}
void Lz4OutputIntOverflow() {
    auto file{MakeNso({0x00}, 0x80000000, true)};
    Rejects([&] { Load(file); }, "LZ4");
}
void Lz4InputIntOverflow() {
    auto file{MakeNso({}, 1, true)};
    Write32(file->bytes, 0x60, 0x80000000);
    file->size = 0x80000100ULL;
    Rejects([&] { Load(file); }, "LZ4");
}
void SymbolRangeOverflow() {
    auto file{MakeNso({1}, 1)};
    Write32(file->bytes, 0x90, 0xFFFFFFF0); // dynstr offset
    Write32(file->bytes, 0x94, 0x10);
    Write32(file->bytes, 0x98, 0xFFFFFFF0); // dynsym offset
    Write32(file->bytes, 0x9C, 0x10);
    Load(file);
    Require(capturedExecutable->dynsym.size == 0 && capturedExecutable->dynstr.size == 0, "overflowing symbol ranges accepted");
}
void RoShortHeader() { NsoLoader::PrintRoContentsInfo(std::vector<u8>(4)); }
void RoUnterminated() {
    std::vector<u8> ro(11);
    Write32(ro, 4, 3);
    ro[8] = 'a'; ro[9] = 'b'; ro[10] = 'c';
    hostLogs.clear();
    NsoLoader::PrintRoContentsInfo(ro);
    Require(std::find(hostLogs.begin(), hostLogs.end(), "Module Path: abc") != hostLogs.end(), "module path not bounded by declared length");
}
void RoInvalidLength() {
    std::vector<u8> ro(9);
    Write32(ro, 4, 100);
    ro[8] = 'x';
    hostLogs.clear();
    NsoLoader::PrintRoContentsInfo(ro);
    Require(std::find(hostLogs.begin(), hostLogs.end(), "Module Path: ") != hostLogs.end(), "invalid module path length accepted");
}
}
int main(int argc, char **argv) {
    // Prevent malformed u32 lengths from OOM-killing the host before validation runs.
#ifndef __SANITIZE_ADDRESS__
    const rlimit memoryLimit{512ULL * 1024 * 1024, 512ULL * 1024 * 1024};
    setrlimit(RLIMIT_AS, &memoryLimit);
#endif
    const std::pair<const char *, void(*)()> tests[]{
        {"raw", Raw}, {"lz4", Lz4}, {"zbic", Zbic}, {"invalid_lz4", InvalidLz4}, {"short_lz4", ShortLz4},
        {"zero_compressed_size", ZeroCompressedSize}, {"short_read", ShortRead},
        {"header_short_read", HeaderShortRead}, {"range_before_allocation", RangeBeforeAllocation}, {"lz4_output_int_overflow", Lz4OutputIntOverflow},
        {"lz4_input_int_overflow", Lz4InputIntOverflow}, {"symbol_range_overflow", SymbolRangeOverflow},
        {"ro_short_header", RoShortHeader}, {"ro_unterminated", RoUnterminated}, {"ro_invalid_length", RoInvalidLength}
    };
    unsigned failures{};
    for (const auto &[name, test] : tests) {
        if (argc > 1 && name != std::string_view(argv[1])) continue;
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; ++failures; }
    }
    return failures ? 1 : 0;
}
