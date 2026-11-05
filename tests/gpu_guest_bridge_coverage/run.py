#!/usr/bin/env python3
"""Compile and execute bounded native encoder-coverage byte oracles.

The production mixed-authority gate remains OFF. This standalone fixture proves
byte addresses only; it does not validate Vulkan synchronization or ownership.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / "tests/gpu_guest_bridge_coverage/tests.cpp"
compiler = os.getenv("CXX", "g++")
sanitizers = os.getenv("SANITIZE", "address,undefined")
with tempfile.TemporaryDirectory(prefix="strato-guest-bridge-") as tmp:
    executable = Path(tmp) / "guest_bridge_coverage"
    command = [compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-fno-omit-frame-pointer", str(source), "-o", str(executable)]
    if sanitizers:
        command += [f"-fsanitize={sanitizers}"]
    subprocess.run(command, cwd=root, check=True)
    env = dict(os.environ)
    if "address" in sanitizers:
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(executable)], cwd=root, env=env, check=True)
