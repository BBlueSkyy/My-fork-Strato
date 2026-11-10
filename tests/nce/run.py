#!/usr/bin/env python3
"""Compile and run standalone NCE acquire and line-width regression tests."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent

with tempfile.TemporaryDirectory(prefix="strato-nce-") as directory:
    for name in ("acquire_lock_test.cpp", "line_width_compat_test.cpp"):
        source = HERE / name
        binary = Path(directory) / source.stem
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-g",
                        "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
        print("PASS:", source.name, flush=True)

