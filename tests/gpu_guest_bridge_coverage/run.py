#!/usr/bin/env python3
"""Compile bounded byte-coverage and transactional writer-ledger oracles.

The production mixed-authority gate remains OFF. These fixtures prove byte
geometry and metadata transitions, not actual GPU readback or Vulkan sync.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = os.getenv("CXX", "g++")
sanitizers = os.getenv("SANITIZE", "address,undefined")
sources = [
    root / "tests/gpu_guest_bridge_coverage/tests.cpp",
    root / "tests/gpu_guest_bridge_coverage/ledger_tests.cpp",
]
with tempfile.TemporaryDirectory(prefix="strato-guest-bridge-") as tmp:
    for source in sources:
        executable = Path(tmp) / source.stem
        command = [compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
                   "-Werror", "-fno-omit-frame-pointer", str(source),
                   "-o", str(executable)]
        if sanitizers:
            command += [f"-fsanitize={sanitizers}"]
        subprocess.run(command, cwd=root, check=True)
        env = dict(os.environ)
        if "address" in sanitizers:
            env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        subprocess.run([str(executable)], cwd=root, env=env, check=True)
