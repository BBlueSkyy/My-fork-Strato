#!/usr/bin/env python3
"""Focused standalone mapping/index regressions; deliberately not required by APK CI."""
import os
import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="strato-texman-multirange-") as directory:
    for name in ("mapping_match", "overlap_index", "test"):
        executable = pathlib.Path(directory) / name
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   str(HERE / (name + ".cpp")), "-o", str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
        print("PASS:", name, flush=True)
