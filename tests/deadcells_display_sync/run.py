#!/usr/bin/env python3
# Native test; no Android SDK, Vulkan or GPU required.
import os
import pathlib
import subprocess
import tempfile

test = pathlib.Path(__file__).with_name("test.cpp")
with tempfile.TemporaryDirectory(prefix="deadcells_display_sync_") as tmp:
    out = pathlib.Path(tmp) / "test"
    subprocess.run([
        os.environ.get("CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
        "-Werror", "-pedantic", "-UNDEBUG", str(test), "-o", str(out)], check=True)
    subprocess.run([str(out)], check=True)
