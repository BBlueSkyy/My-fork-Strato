#!/usr/bin/env python3
"""Host regression for the compiled PR-only Oceanhorn telemetry header.

Compiles the actual production header with a minimal logger façade, checks
intermediate report suppression, interval counters, fallback/sync deltas and
bounded shader/material fingerprints. No Android/GPU runtime needed.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "app/src/main/cpp/skyline/gpu/diagnostics/oceanhorn_render.h"

LOGGER = r'''#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace diagnostic_test {
struct Log { std::string format; std::vector<uint64_t> args; };
inline std::vector<Log> entries;
template <class... T> void Record(const char *fmt, T... values) {
    entries.push_back({fmt, {static_cast<uint64_t>(values)...}});
}
}
#define LOGI(...) ::diagnostic_test::Record(__VA_ARGS__)
'''

CPP = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <gpu/diagnostics/oceanhorn_render.h>

using namespace skyline::gpu::diagnostics::oceanhorn;

const std::vector<uint64_t> &get(const char *marker) {
    auto it = std::find_if(diagnostic_test::entries.begin(),
                           diagnostic_test::entries.end(),
                           [marker](const auto &item) {
                               return item.format.find(marker) == 0;
                           });
    assert(it != diagnostic_test::entries.end());
    return it->args;
}

int main() {
    RenderSummary summary;
    DrawObservation observed{};
    observed.shaderKey = 0x1234;
    observed.materialKey = 0xAABB;
    observed.elements = 12;
    observed.instances = 2;
    observed.textureSlots = 2;
    observed.mip0 = 1;
    observed.mip1 = 1;
    observed.singleMip = 1;
    observed.hasColor = true;
    summary.Record(observed);

    Events::fallbackUnmapped.fetch_add(3);
    Events::nullBindlessSampled.fetch_add(4);
    Events::samplersCreated.fetch_add(1);
    Events::guestToHostUploads.fetch_add(2);
    Events::guestToHostBytes.fetch_add(4096);
    Events::mipGrowths.fetch_add(1);
    Events::mipLevelsAdded.fetch_add(2);
    summary.Report(15);
    assert(diagnostic_test::entries.empty());
    summary.Report(16);
    assert(get("[OCEAN-GEO]")[2] == 1);
    assert(get("[OCEAN-GEO]")[8] == 24);
    assert(get("[OCEAN-FALLBACK]")[3] == 3);
    assert(get("[OCEAN-FALLBACK]")[5] == 4);
    assert(get("[OCEAN-LOD]")[1] == 1);
    assert(get("[OCEAN-SYNC]")[1] == 2);
    assert(get("[OCEAN-SYNC]")[2] == 4096);
    assert(get("[OCEAN-SYNC]")[7] == 1);
    assert(get("[OCEAN-SYNC]")[8] == 2);
    assert(get("[OCEAN-MIPS]")[1] == 1);
    assert(get("[OCEAN-MIPS]")[2] == 1);
    assert(get("[OCEAN-MIPS]")[5] == 1);
    assert(get("[OCEAN-MATERIAL]")[1] == 0xAABB);
    assert(get("[OCEAN-MATERIAL]")[2] == 1);

    diagnostic_test::entries.clear();
    summary.Record(observed);
    summary.Report(110); // 110 - 16 < 96
    assert(diagnostic_test::entries.empty());
    summary.Report(112);
    assert(get("[OCEAN-GEO]")[2] == 1);
    assert(get("[OCEAN-FALLBACK]")[3] == 0); // delta reset
    assert(get("[OCEAN-SYNC]")[1] == 0);
    assert(get("[OCEAN-MATERIAL]")[1] == 0xAABB);
    assert(get("[OCEAN-MATERIAL]")[2] == 1);

    diagnostic_test::entries.clear();
    for (uint64_t i{}; i < 50; ++i) {
        observed.shaderKey = i + 1;
        observed.materialKey = i + 100;
        summary.Record(observed);
    }
    summary.Report(208);
    assert(get("[OCEAN-SHADERS]").back() == 18);
    assert(get("[OCEAN-MATERIAL]").back() == 18);
}
'''

with tempfile.TemporaryDirectory(prefix="strato-ocean-diag-") as tmp:
    folder = pathlib.Path(tmp)
    (folder / "logger").mkdir()
    (folder / "logger/logger.h").write_text(LOGGER)
    (folder / "test.cpp").write_text(CPP)
    args = ["g++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I" + str(folder), "-I" + str(ROOT / "app/src/main/cpp"),
            str(folder / "test.cpp"), "-o", str(folder / "check")]
    subprocess.run(args, check=True)
    subprocess.run([str(folder / "check")], check=True)
print("PASS Oceanhorn diagnostic report intervals, fallback/sync deltas, mip/material bins")
