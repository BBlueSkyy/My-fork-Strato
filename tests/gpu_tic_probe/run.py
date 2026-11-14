#!/usr/bin/env python3
"""Executable TIC probe helper tests plus a check that production calls the probe."""
import os
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
src = root / "app/src/main/cpp/skyline"
cpp = (src / "gpu/interconnect/common/textures.cpp").read_text()
graphics = (src / "gpu/interconnect/maxwell_3d/pipeline_manager.cpp").read_text()
compute = (src / "gpu/interconnect/kepler_compute/pipeline_manager.cpp").read_text()
pool = (src / "gpu/interconnect/common/textures.h").read_text()

for token in ("SnapshotWords(tic)", "slotContiguousBytes", "imageBaseMapped",
              "channelSequenceNumber", "GetPoolFlushGeneration",
              "LogTicProbe(ctx, pool, index, textureHeader, resident, \"compile\", 2)",
              "LogTicProbe(ctx, pool, index, textureHeader, resident, \"sampled\", 1)",
              "ConsumeRecovered(pool.GetPoolIova(), index)"):
    # Diagnostic schema and entry points must not silently disappear.
    assert token in cpp, token

assert 'ScopedShaderContext shaderProbe{"graphics"' in graphics
assert 'ScopedShaderContext shaderProbe{"compute"' in compute
assert "GetPoolIova() const" in pool

with tempfile.TemporaryDirectory(prefix="strato-tic-probe-") as tmp:
    exe = pathlib.Path(tmp) / "tic_probe_test"
    flags = [] if os.environ.get("SANITIZE") == "0" else [
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-g", "-Wall",
        "-Wextra", "-Werror", *flags, f"-I{src}",
        str(pathlib.Path(__file__).with_name("tests.cpp")),
        "-o", str(exe)
    ], check=True)
    env = os.environ.copy()
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(exe)], env=env, check=True)
print("TIC probe production wiring: PASS")
