#!/usr/bin/env python3
# Narrow native regression, without Android SDK or GPU requirements.
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
test = pathlib.Path(__file__).with_name("test.cpp")
with tempfile.TemporaryDirectory(prefix="deadcells_memory_origin_") as tmp:
    out = pathlib.Path(tmp) / "test"
    subprocess.run([
        os.environ.get("CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
        "-Werror", "-pedantic", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I", str(root / "app/src/main/cpp/skyline"),
        str(test), "-o", str(out)], check=True)
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(out)], check=True, env=env)
