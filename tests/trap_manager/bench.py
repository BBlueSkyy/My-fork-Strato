#!/usr/bin/env python3
"""Compare TrapManager protection-path cost across git refs (and the working tree) with the same workload.

    python3 tests/trap_manager/bench.py d94289e a04e1c2 WORKTREE

Counts the real mprotect() calls through the linker's --wrap. Reuses the stand-in headers of run.py. Not part of CI.
"""
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMMON = "app/src/main/cpp/skyline/common"
source = (ROOT / "tests/trap_manager/run.py").read_text()
stand_ins = {name: re.search(name.upper() + r" = r?'''(.*?)'''", source, re.S) for name in ("UTILS", "SPAN")}
UTILS = stand_ins["UTILS"].group(1)
SPAN = stand_ins["SPAN"].group(1)
TRACE = "#pragma once\n#include \"utils.h\"\n#define TRACE_EVENT(...) do {} while (0)\n"

refs = sys.argv[1:] or ["WORKTREE"]
with tempfile.TemporaryDirectory(prefix="strato-trap-bench-") as directory:
    for ref in refs:
        build = pathlib.Path(directory) / ref.replace("/", "_")
        build.mkdir()
        for name in ("interval_map.h", "trap_manager.h", "trap_manager.cpp"):
            if ref == "WORKTREE":
                shutil.copy(ROOT / COMMON / name, build / name)
            else:
                (build / name).write_text(subprocess.run(["git", "-C", str(ROOT), "show", f"{ref}:{COMMON}/{name}"], check=True, capture_output=True, text=True).stdout)
        (build / "utils.h").write_text(UTILS)
        (build / "span.h").write_text(SPAN)
        (build / "trace.h").write_text(TRACE)
        binary = build / "bench"
        subprocess.run(["g++", "-std=c++20", "-O2", "-DNDEBUG", "-Wno-invalid-offsetof", "-pthread", "-Wl,--wrap=mprotect", "-I", str(build),
                        str(ROOT / "tests/trap_manager/bench.cpp"), str(build / "trap_manager.cpp"), "-o", str(binary)], check=True)
        import os
        for label, env in (("with mprotect syscalls", {}), ("bookkeeping only (mprotect stubbed)", {"STRATO_BENCH_NOSYSCALL": "1"})):
            print(f"== {ref}: {label}")
            best = {}
            for _ in range(5):  # keep the fastest of 5 runs per line to filter scheduler noise
                out = subprocess.run([str(binary)], check=True, capture_output=True, text=True, env={**os.environ, **env}).stdout.splitlines()
                for line in out:
                    key = line[:26]
                    nums = [float(x) for x in re.findall(r"(\d+) ns", line)]
                    if key not in best or sum(nums) < best[key][0]:
                        best[key] = (sum(nums), line)
            for _, line in best.values():
                print(line)
