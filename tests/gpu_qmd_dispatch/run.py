#!/usr/bin/env python3
"""Prove dispatch-limit boundary semantics and diagnostic-only integration."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / "app/src/main/cpp/skyline/gpu/interconnect/kepler_compute"
compute = (source / "kepler_compute.cpp").read_text()
engine = (root / "app/src/main/cpp/skyline/soc/gm20b/engines/kepler_compute.cpp").read_text()
header = (source / "kepler_compute.h").read_text()

assert 'GetComputeDispatchLimits(gpu.vkPhysicalDevice)' in compute
assert 'IsComputeGridWithinLimits(grid, maxComputeWorkGroupCount)' in compute
assert compute.index('IsComputeGridWithinLimits(grid, maxComputeWorkGroupCount)') < compute.index('ctx.executor.RunWithTextureBackingRetry')
assert 'Rejecting invalid compute dispatch' in compute
assert 'std::memory_order_relaxed' in compute
assert 'std::memcmp(&qmd, &again, sizeof(qmd))' in engine
assert 'interconnect.Dispatch(qmd, qmdAddress)' in engine
assert 'void Dispatch(const QMD &qmd, u64 qmdAddress)' in header
assert 'std::clamp' not in compute

harness = r"""
#include "dispatch_limits.h"
using skyline::gpu::interconnect::kepler_compute::IsComputeGridWithinLimits;
static_assert(IsComputeGridWithinLimits({0, 0, 0}, {65535, 65535, 65535}));
static_assert(IsComputeGridWithinLimits({65535, 65535, 65535}, {65535, 65535, 65535}));
static_assert(IsComputeGridWithinLimits({1, 1, 1}, {65535, 1024, 1024}));
static_assert(!IsComputeGridWithinLimits({65536, 1, 1}, {65535, 65535, 65535}));
static_assert(!IsComputeGridWithinLimits({1, 1025, 1}, {65535, 1024, 1024}));
static_assert(!IsComputeGridWithinLimits({1, 1, 1025}, {65535, 1024, 1024}));
static_assert(!IsComputeGridWithinLimits({4130713328U, 1, 1}, {65535, 65535, 65535}));
int main() { return 0; }
"""
with tempfile.TemporaryDirectory(prefix="qmd-limits-") as td:
    td = Path(td)
    (td / "test.cpp").write_text(harness)
    cmd = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
           "-pedantic", "-I", str(source), str(td / "test.cpp"), "-o", str(td / "test")]
    subprocess.run(cmd, check=True)
    subprocess.run([str(td / "test")], check=True, timeout=10)

print("PASS: C++20 grid-limit boundaries and source-level early diagnostic guard")
