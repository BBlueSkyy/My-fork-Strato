#!/usr/bin/env python3
# Native test for the slot-release generation guard; no Android SDK required.
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = pathlib.Path(__file__).with_name("slot_release_test.cpp")
with tempfile.TemporaryDirectory(prefix="hosbinder_slot_release_") as work:
    binary = pathlib.Path(work) / "slot_release_test"
    subprocess.run([
        os.environ.get("CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
        "-Werror", "-pedantic", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I", str(root / "app/src/main/cpp/skyline"),
        str(source), "-o", str(binary)], check=True)
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(binary)], check=True, env=env)
