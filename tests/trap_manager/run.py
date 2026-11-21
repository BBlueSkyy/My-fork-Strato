#!/usr/bin/env python3
"""Build the production TrapManager/IntervalMap against real mprotect'ed pages and check page protections.

The repository sources are copied unmodified into a scratch directory next to minimal stand-ins for the Android-only headers
(utils.h, span.h, trace.h), so no NDK, perfetto or fmt is needed. Use --sanitize for ASan/UBSan.
"""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMMON = ROOT / "app/src/main/cpp/skyline/common"

UTILS = r'''
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace skyline {
    using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
    namespace constant { constexpr size_t PageSize{0x1000}; }
    namespace util {
        template<typename T, typename M> constexpr T AlignDown(T value, M multiple) {
            if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(value) / multiple * multiple);
            else return static_cast<T>(value / multiple * multiple);
        }
        template<typename T, typename M> constexpr T AlignUp(T value, M multiple) {
            if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>((reinterpret_cast<uintptr_t>(value) + multiple - 1) / multiple * multiple);
            else return static_cast<T>((value + multiple - 1) / multiple * multiple);
        }
    }
}
'''
SPAN = r'''
#pragma once
#include <span>
#include "utils.h"
namespace skyline {
    template<typename T, size_t Extent = std::dynamic_extent>
    class span : public std::span<T, Extent> {
      public:
        using std::span<T, Extent>::span;
        constexpr span(const std::span<T, Extent> &other) : std::span<T, Extent>(other) {}
        bool valid() const { return this->data() != nullptr; }
    };
}
'''
TRACE = "#pragma once\n#include \"utils.h\"\n#define TRACE_EVENT(...) do {} while (0)\n"

parser = argparse.ArgumentParser()
parser.add_argument("--sanitize", action="store_true", help="build with ASan+UBSan")
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="strato-trap-manager-") as directory:
    build = pathlib.Path(directory)
    for name in ("interval_map.h", "trap_manager.h", "trap_manager.cpp"):
        shutil.copy(COMMON / name, build / name)
    (build / "common").mkdir()
    shutil.copy(COMMON / "oh2_perf.h", build / "common" / "oh2_perf.h")
    (build / "utils.h").write_text(UTILS)
    (build / "span.h").write_text(SPAN)
    (build / "trace.h").write_text(TRACE)
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wno-invalid-offsetof", "-Wno-unused-parameter", "-O1", "-g", "-pthread"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    binary = build / "trap_manager_tests"
    subprocess.run(["g++", *flags, "-I", str(build), str(ROOT / "tests/trap_manager/tests.cpp"), str(build / "trap_manager.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
