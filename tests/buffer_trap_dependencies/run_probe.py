#!/usr/bin/env python3
"""Compile the actual optional reporter with the production logger API and fmt."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--deps", type=Path, default=Path("app/libraries"))
args = parser.parse_args()
here = Path(__file__).resolve().parent
cpp = here.parents[1] / "app/src/main/cpp/skyline"
with tempfile.TemporaryDirectory(prefix="strato-buffer-probe-") as td:
    binary = str(Path(td) / "probe")
    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-pthread",
               "-Wall", "-Wextra", "-Werror", "-DSTRATO_BUFFER_READBACK_PROBE=1",
               "-DFMT_HEADER_ONLY", "-fsanitize=address,undefined",
               "-fno-sanitize-recover=all", "-I" + str(cpp),
               "-I" + str(args.deps.resolve() / "fmt/include"),
               str(here / "probe.cpp"), str(cpp / "gpu/buffer_readback_probe.cpp"),
               "-o", binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, timeout=30)
