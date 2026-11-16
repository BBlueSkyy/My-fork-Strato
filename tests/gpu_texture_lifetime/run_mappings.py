#!/usr/bin/env python3
"""Essential physical mapping journal/concurrency regressions."""
import os
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="strato-texture-mappings-") as directory:
    binary = pathlib.Path(directory) / "tests"
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g",
        "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        "-I" + str(ROOT / "app/src/main/cpp/skyline"),
        str(ROOT / "tests/gpu_texture_lifetime/physical_mappings.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
