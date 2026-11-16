#!/usr/bin/env python3
"""Essential physical mapping journal/concurrency regressions."""
import os
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="strato-texture-mappings-") as directory:
    temporary = pathlib.Path(directory)
    (temporary / "common").mkdir()
    # Only the pre-existing flat translation allocator is a host boundary. The
    # production GMMU wrapper, mapping tracker and ChannelContext methods are real.
    (temporary / "common/address_space.h").write_text(r"""
#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
namespace skyline {
using u8 = uint8_t; using u64 = uint64_t;
struct MemoryManagerBlockInfo { bool sparseMapped{}; };
template<class V, V U, size_t A, size_t S, size_t B> struct FlatMemoryManager {
    unsigned mapCalls{}, unmapCalls{};
    bool failNext{};
    static u8 *SparsePlaceholderAddress() { return reinterpret_cast<u8 *>(1); }
    void Map(u64, u8 *, u64, MemoryManagerBlockInfo = {}) {
        if (failNext) { failNext = false; throw std::bad_alloc{}; }
        ++mapCalls;
    }
    void Unmap(u64, u64) { ++unmapCalls; }
};
}
""")
    def function(text, signature):
        start = text.index(signature)
        end = text.index("{", start) + 1
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        return text[start:end]
    channel = (ROOT / "app/src/main/cpp/skyline/soc/gm20b/channel.cpp").read_text()
    (temporary / "channel_collection.inc").write_text("\\n".join(function(channel, signature)
        for signature in ("void ChannelContext::Lock(", "void ChannelContext::Unlock(")))
    binary = temporary / "tests"
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        "-I" + str(temporary), "-I" + str(ROOT / "app/src/main/cpp/skyline"),
        str(ROOT / "tests/gpu_texture_lifetime/physical_mappings.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
