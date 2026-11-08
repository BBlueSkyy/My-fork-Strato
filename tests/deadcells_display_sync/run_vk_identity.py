#!/usr/bin/env python3
# Native C++20 regression (no Android/Vulkan SDK required for formatter).
import os
import pathlib
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[2]
test = pathlib.Path(__file__).with_name("test_vk_identity.cpp")
with tempfile.TemporaryDirectory(prefix="deadcells_vk_labels_") as tmp:
    output = pathlib.Path(tmp) / "vk_labels"
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20",
        "-Wall", "-Wextra", "-Werror", "-pedantic", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I", str(root / "app/src/main/cpp/skyline"),
        str(test), "-o", str(output)], check=True)
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(output)], env=env, check=True)
