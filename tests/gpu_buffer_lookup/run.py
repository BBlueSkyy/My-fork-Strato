#!/usr/bin/env python3
"""Run actual BufferManager lookup with host range/page-table/lock facades.

--benchmark measures only a forced fallback CPU fixture, not Android gameplay.
BUFFER_LOOKUP_SOURCE may select an unmodified baseline .cpp for comparison.
"""
import argparse
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("--benchmark", action="store_true")
args = parser.parse_args()


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n    }", start) + 6]


source = pathlib.Path(os.environ.get("BUFFER_LOOKUP_SOURCE", str(ROOT / "app/src/main/cpp/skyline/gpu/buffer_manager.cpp"))).read_text()
with tempfile.TemporaryDirectory(prefix="strato-buffer-lookup-") as temporary:
    directory = pathlib.Path(temporary)
    (directory / "production.inc").write_text("\n".join(method(source, signature) for signature in (
        "    bool BufferManager::BufferLessThan(",
        "    BufferManager::LockedBuffers BufferManager::Lookup(")))
    flags = ["-O3", "-DNDEBUG"] if args.benchmark else ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", *flags,
                    "-I" + str(directory), str(ROOT / "tests/gpu_buffer_lookup/tests.cpp"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests"), *(["--benchmark"] if args.benchmark else [])], check=True)
