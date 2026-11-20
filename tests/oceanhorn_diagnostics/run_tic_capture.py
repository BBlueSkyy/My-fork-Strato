#!/usr/bin/env python3
"""Host regression for the PR-only unmapped-TIC capture header.

Compiles the production header (oceanhorn_tic_capture.h) with a minimal logger
façade. Covers GMMU segment analysis (partial / fully unmapped / holes), per-descriptor
aggregation instead of per-lookup rows, bounded tables, re-analysis detecting mapping
changes, resolution/rebind tracking and report throttling. No Android/GPU runtime.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

LOGGER = r'''#pragma once
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>
namespace diagnostic_test {
struct Log { std::string format; std::vector<uint64_t> args; };
inline std::vector<Log> entries;
template <class T> uint64_t Numeric(const T &value) {
    if constexpr (std::is_arithmetic_v<T>) return static_cast<uint64_t>(value);
    else return 0;
}
template <class... T> void Record(const char *fmt, const T &...values) {
    entries.push_back({fmt, {Numeric(values)...}});
}
}
#define LOGI(...) ::diagnostic_test::Record(__VA_ARGS__)
'''

CPP = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <gpu/diagnostics/oceanhorn_tic_capture.h>

using namespace skyline::gpu::diagnostics::oceanhorn;
using Segments = std::vector<std::pair<bool, uint64_t>>;

static size_t Count(const char *marker) {
    size_t n{};
    for (const auto &e : diagnostic_test::entries)
        if (e.format.rfind(marker, 0) == 0) ++n;
    return n;
}

static const std::vector<uint64_t> &First(const char *marker) {
    for (const auto &e : diagnostic_test::entries)
        if (e.format.rfind(marker, 0) == 0) return e.args;
    assert(false);
    static const std::vector<uint64_t> none;
    return none;
}

static TicDescriptor Desc(uint32_t index, uint64_t iova) {
    TicDescriptor d{};
    d.index = index;
    d.iova = iova;
    d.backingIova = iova;
    d.formatWord = 0x58D24944;
    d.width = d.height = 256;
    d.depth = 1;
    d.totalMips = 9;
    d.viewMipBase = 2;
    d.viewMipCount = 7;
    d.shaderType = 1;
    return d;
}

using Pair = std::pair<MappingAnalysis, MappingAnalysis>;

int main() {
    // --- segment analysis -------------------------------------------------
    {
        auto full = AnalyzeSegments(Segments{{true, 0x10000}});
        assert(full.Complete() && full.baseMapped && full.requestedBytes == 0x10000);
        assert(full.validPages4k == 16 && full.invalidPages4k == 0);
        assert(full.firstInvalidOffset == MappingAnalysis::NoInvalid);

        // Base page mapped, the tail of the mip chain is not: the partially-mapped case.
        auto partial = AnalyzeSegments(Segments{{true, 0x8000}, {false, 0x8000}});
        assert(!partial.Complete() && partial.baseMapped);
        assert(partial.firstInvalidOffset == 0x8000);
        assert(partial.validBytes == 0x8000 && partial.invalidBytes == 0x8000);
        assert(partial.invalidSegments == 1 && partial.segments == 2);

        // A hole in the middle of otherwise mapped memory.
        auto hole = AnalyzeSegments(Segments{{true, 0x1000}, {false, 0x2000}, {true, 0x1000}});
        assert(hole.firstInvalidOffset == 0x1000 && hole.invalidPages4k == 2 && hole.validPages4k == 2);

        // Nothing mapped at all: the base address itself is unmapped.
        auto none = AnalyzeSegments(Segments{{false, 0x4000}});
        assert(!none.baseMapped && none.firstInvalidOffset == 0 && none.validBytes == 0);

        auto empty = AnalyzeSegments(Segments{});
        assert(!empty.Complete() && !empty.baseMapped);
        assert(HashShaderStages(std::vector<uint64_t>{1, 2}) != HashShaderStages(std::vector<uint64_t>{2, 1}));
    }

    auto &capture = UnmappedTicCapture::Instance();
    int analyses{};
    auto partial = AnalyzeSegments(Segments{{true, 0x8000}, {false, 0x8000}});
    auto none = AnalyzeSegments(Segments{{false, 0x10000}});

    // --- a million lookups of one TIC stay one row, analysed once ---------------
    {
        auto d = Desc(10, 0x508452200);
        d.shaderKey = 0xAAAA;
        d.stage = 0x80;
        for (int i = 0; i < 1000; ++i)
            capture.Observe(d, 0x10000, 0x18000, [&] { ++analyses; return Pair{partial, none}; });
        assert(capture.Size() == 1);
        assert(capture.At(0).hits == 1000);
        assert(analyses == 1);
        assert(capture.At(0).minimum.baseMapped && !capture.At(0).minimum.Complete());
        assert(capture.At(0).minimum.firstInvalidOffset == 0x8000);
        assert(!capture.At(0).full.baseMapped);
        assert(capture.At(0).requestedFull == 0x18000);
        assert(Count("[OCEAN-UNMAPPED-NEW]") == 3); // three log lines for one first-seen entry
    }

    // --- a different IOVA at the same TIC index is a different descriptor -------
    {
        auto d = Desc(10, 0x509000000);
        capture.Observe(d, 0x10000, 0x10000, [&] { ++analyses; return Pair{none, none}; });
        assert(capture.Size() == 2 && analyses == 2);
        // Shader/stage differences only add to the entry, they do not split it.
        auto a = Desc(11, 0x1000);
        for (uint64_t shader = 1; shader <= 6; ++shader) {
            a.shaderKey = shader;
            a.stage = 0x10;
            capture.Observe(a, 1, 1, [&] { ++analyses; return Pair{}; });
        }
        assert(capture.Size() == 3);
        assert(capture.At(2).shaderCount == 4 && capture.At(2).shaderOverflow == 2 && capture.At(2).hits == 6);
    }

    // --- a later guest-backed build resolves / rebinds ----------------------------
    {
        capture.NoteBuilt(Desc(10, 0x508452200));
        assert(capture.At(0).resolvedBuilds == 1 && capture.At(0).reboundBuilds == 0);
        // The same TIC index rebuilt from a different address marks the other rows as rebound.
        assert(capture.At(1).reboundBuilds == 1 && capture.At(1).reboundIova == 0x508452200);
        assert(capture.At(2).resolvedBuilds == 0); // different TIC index untouched
    }

    // --- re-analysis catches a mapping that changed during the session -------------
    {
        auto d = Desc(10, 0x508452200);
        const auto before = capture.At(0).hits;
        const auto toReach = UnmappedTicCapture::ReanalyzeInterval - before % UnmappedTicCapture::ReanalyzeInterval;
        auto healed = AnalyzeSegments(Segments{{true, 0x10000}});
        for (uint64_t i = 0; i < toReach; ++i)
            capture.Observe(d, 0x10000, 0x18000, [&] { ++analyses; return Pair{healed, healed}; });
        assert(capture.At(0).mappingChanges == 1);
        assert(capture.At(0).minimum.Complete());
    }

    // --- bounded table -------------------------------------------------------------
    {
        const auto overflowBefore = capture.Overflow();
        for (uint32_t i = 0; i < UnmappedTicCapture::Capacity + 8; ++i)
            capture.Observe(Desc(1000 + i, 0x700000000ull + i * 0x1000), 1, 1, [&] { return Pair{}; });
        assert(capture.Size() == UnmappedTicCapture::Capacity);
        assert(capture.Overflow() > overflowBefore);
        // Known descriptors are still counted once the table is full.
        const auto hits = capture.At(0).hits;
        capture.Observe(Desc(10, 0x508452200), 1, 1, [&] { return Pair{}; });
        assert(capture.At(0).hits == hits + 1);
    }

    // --- report throttling and windowed deltas ----------------------------------------
    {
        diagnostic_test::entries.clear();
        capture.Report(1);
        assert(Count("[OCEAN-UNMAPPED] ") == 1);
        auto &summary = First("[OCEAN-UNMAPPED] ");
        assert(summary[1] == UnmappedTicCapture::Capacity); // distinct
        assert(Count("[OCEAN-UNMAPPED-TOP]") == 8 * 3);

        diagnostic_test::entries.clear();
        capture.Report(2);
        assert(First("[OCEAN-UNMAPPED] ")[3] == 0); // window reset by the previous report

        diagnostic_test::entries.clear();
        capture.Report(9);  // > 8 and not a multiple of 4: suppressed
        assert(diagnostic_test::entries.empty());
        capture.Report(12); // multiple of 4: reported
        assert(Count("[OCEAN-UNMAPPED] ") == 1);
    }
}
'''

with tempfile.TemporaryDirectory(prefix="strato-ocean-tic-") as tmp:
    folder = pathlib.Path(tmp)
    (folder / "logger").mkdir()
    (folder / "logger/logger.h").write_text(LOGGER)
    (folder / "test.cpp").write_text(CPP)
    args = ["g++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I" + str(folder), "-I" + str(ROOT / "app/src/main/cpp/skyline"),
            str(folder / "test.cpp"), "-o", str(folder / "check")]
    subprocess.run(args, check=True)
    subprocess.run([str(folder / "check")], check=True)
print("PASS Oceanhorn unmapped-TIC capture: segment analysis, per-TIC aggregation, bounds, rebinding, report throttling")
