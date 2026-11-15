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
dma = (src / "soc/gm20b/engines/maxwell_dma.cpp").read_text()
i2m = (src / "soc/gm20b/engines/inline2memory.cpp").read_text()
nvhost = (src / "services/nvdrv/devices/nvhost/as_gpu.cpp").read_text()
pipeline = (src / "gpu/interconnect/common/pipeline.inc").read_text()
maxwell = (src / "gpu/interconnect/maxwell_3d/maxwell_3d.cpp").read_text()
buffer = (src / "gpu/buffer.h").read_text()

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
assert "WatchFirstTic(" in cpp
assert 'engine=MaxwellDMA' in dma and 'suspectConstant' in dma
assert "OverlapsWatchedFirstTic" in dma
assert 'engine=I2M' in i2m and "poisonHeader" in i2m
assert "OverlapsWatchedFirstTic" in i2m
assert "nvMapMatch" in cpp and "nvMapBackingAddress" in cpp
assert "FindMapping(ctx.channelCtx.asCtx.get()" in cpp
assert "RememberMapping(" in nvhost and "ForgetMappingRange(" in nvhost
assert "MappingSource::SparseRemap" in nvhost
assert "RememberZeroBindless" in pipeline and "if (!primaryCbuf.view)" in pipeline
assert "bindlessMatched" in cpp and "primaryBound" in cpp
assert "ProbeKirbyFragmentP0(ctx, *pipeline" in maxwell
assert "sourcePackedState.shaderHashes[5]" in maxwell
assert "PredicateCbufIndex{3}" in maxwell and "PredicateByteOffset{0x04}" in maxwell
assert "TryPeekDiagnosticWord" in maxwell
assert "TryPeekDiagnosticWord" in buffer and "currentExecutionGpuDirty" in buffer
assert "directGpuWritesActive || directTrackedShadowActive" in buffer
assert "dirtyState == DirtyState::GpuDirty" in buffer
assert "gpu-visible-not-shader-execution" not in maxwell  # no claim of device-side evidence
assert "cpu-visible-not-shader-execution" in maxwell

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
