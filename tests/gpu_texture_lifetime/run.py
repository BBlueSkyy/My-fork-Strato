#!/usr/bin/env python3
"""Reuse #461 production-body probes, now with the real TrapManager/IntervalMap.

GPU and mirror interfaces in probes.cpp remain explicit host facades.
The separate backing-promotion lane validates actual Vulkan resources.
"""
import argparse
import hashlib
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMMON = "app/src/main/cpp/skyline/common/"
TEXTURE = "app/src/main/cpp/skyline/gpu/texture/texture.cpp"

def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", help="expect the real callback-final-owner deadlock at this ref")
    parser.add_argument("--sanitizer", default="address,undefined")
    args = parser.parse_args()
    def source(path):
        if args.baseline:
            return subprocess.check_output(["git", "show", args.baseline + ":" + path], cwd=ROOT, text=True)
        return (ROOT / path).read_text()
    with tempfile.TemporaryDirectory(prefix="strato-texture-lifetime-") as temporary:
        directory = pathlib.Path(temporary)
        for name in ("trap_manager.cpp", "trap_manager.h", "interval_map.h"):
            text = source(COMMON + name)
            (directory / name).write_text(text)
            print(name + " sha256=" + hashlib.sha256(text.encode()).hexdigest(), flush=True)
        # Host boundary: alignment, scalar types, span convenience, tracing.
        # The complete production interval algorithms and trap implementation are unchanged.
        (directory / "utils.h").write_text(r"""
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
namespace skyline {
using u8 = std::uint8_t;
namespace constant { inline constexpr size_t PageSize{4096}; }
namespace util {
template<class T> T *AlignDown(T *p, size_t a) {
    return reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(p) & ~(a - 1));
}
template<class T> T *AlignUp(T *p, size_t a) {
    return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(p) + a - 1) & ~(a - 1));
}
}
}
""")
        (directory / "span.h").write_text(r"""
#pragma once
#include <span>
#include <vector>
namespace skyline {
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::span<T> s) : std::span<T>{s} {}
    operator std::vector<T>() const { return {this->begin(), this->end()}; }
};
}
""")
        (directory / "trace.h").write_text("#pragma once\n#define TRACE_EVENT(...) ((void)0)\n")
        common = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-Wno-invalid-constexpr", "-O1", "-g", "-pthread",
            "-fno-omit-frame-pointer", "-fsanitize=" + args.sanitizer,
            "-fno-sanitize-recover=all", "-I" + str(directory),
            str(directory / "trap_manager.cpp")]
        common += shlex.split(os.environ.get("CXXFLAGS", ""))
        binary = directory / "traps"
        flags = ["-DTRAP_HAS_OWNER"] if "std::weak_ptr<void> owner" in source(COMMON + "trap_manager.h") else []
        if "bool protectionFault" in source(COMMON + "trap_manager.h"):
            flags += ["-DTRAP_HAS_PROTECTION_RETRY"]
        subprocess.run(common + flags + [str(ROOT / "tests/gpu_texture_lifetime/trap_tests.cpp"),
            "-o", str(binary)], check=True)
        if args.baseline:
            try:
                result = subprocess.run([str(binary), "one"], timeout=4, capture_output=True, text=True)
            except subprocess.TimeoutExpired:
                print("CONFIRMED baseline: real TrapManager deadlocks when callback releases final owner", flush=True)
                return
            if result.returncode != 0 and "deadlock after final callback owner" in result.stderr:
                print("CONFIRMED baseline: real TrapManager final-owner callback exceeded bounded completion deadline", flush=True)
                return
            raise RuntimeError("baseline did not reproduce expected deadlock: " + result.stdout + result.stderr)
        subprocess.run([str(binary)], check=True, timeout=30)
        gpu_header = (ROOT / "app/src/main/cpp/skyline/gpu.h").read_text()
        ordered = ["cache::RenderPassCache renderPassCache", "cache::FramebufferCache framebufferCache",
            "CommandScheduler scheduler", "TextureManager texture"]
        assert [gpu_header.index(member) for member in ordered] == sorted(
            gpu_header.index(member) for member in ordered), "caches must outlive scheduler and texture manager"
        text = source(TEXTURE)
        assert "}, weakThis);" in function(text, "void Texture::SetupGuestMappings(")
        (directory / "production.inc").write_text("\n".join(function(text, signature) for signature in (
            "void Texture::SynchronizeGuest(", "Texture::~Texture(")) + "\n")
        print("texture.cpp sha256=" + hashlib.sha256(text.encode()).hexdigest(), flush=True)
        binary = directory / "lifetime"
        subprocess.run(common + [str(ROOT / "tests/gpu_texture_lifetime/probes.cpp"),
            "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)

if __name__ == "__main__":
    main()
