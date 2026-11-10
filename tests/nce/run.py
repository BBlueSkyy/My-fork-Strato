#!/usr/bin/env python3
"""Compile and run every standalone NCE/BOTW probe test and present-copy harness."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

with tempfile.TemporaryDirectory(prefix="strato-nce-") as directory:
    for source in sorted(HERE.glob("*_test.cpp")):
        if source.name == "botw_present_copy_test.cpp":
            continue  # Requires the extracted production function below.
        binary = Path(directory) / source.stem
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-g",
                        "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
        print("PASS:", source.name, flush=True)

subprocess.run([os.environ.get("PYTHON", "python3"), str(HERE / "run_present_copy.py")],
               cwd=ROOT, check=True)
