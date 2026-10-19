#!/usr/bin/env python3
"""Exercise production sampler pool/cache code with host Vulkan interfaces."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu/interconnect/common"

with tempfile.TemporaryDirectory(prefix="strato-samplers-") as directory:
    tmp = Path(directory)
    for source, destination, marker in (
        (ROOT / "app/src/main/cpp/skyline/common/dirty_tracking.h", "dirty.inc", "namespace skyline::dirty"),
        (GPU / "tsc.h", "tsc.inc", "namespace skyline::gpu::interconnect"),
        (GPU / "samplers.h", "samplers_header.inc", "namespace skyline::gpu::interconnect"),
        (GPU / "samplers.cpp", "samplers_source.inc", "namespace skyline::gpu::interconnect"),
        (GPU / "dynamic_pool.h", "dynamic_pool.inc", "namespace skyline::gpu::interconnect"),
        (ROOT / "app/src/main/cpp/skyline/common/interval_list.h", "intervals.inc", "namespace skyline"),
    ):
        text = source.read_text()
        # GCC lacks Clang's nested member designator extension. Change only
        # initializer spelling, preserving the value and all sampler/cache code.
        text = text.replace(".customBorderColor.float32 = {{", ".customBorderColor = {.float32 = {{")
        text = text.replace("texSampler.borderColorA}},", "texSampler.borderColorA}}},")
        (tmp / destination).write_text(text[text.index(marker):])
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-type-limits", "-Wno-return-type", "-Wno-reorder",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I" + str(tmp),
                    str(Path(__file__).with_name("tests.cpp")), "-o", str(tmp / "tests")], check=True)
    subprocess.run([str(tmp / "tests")], check=True)
