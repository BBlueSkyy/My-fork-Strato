#!/usr/bin/env python3
"""Compile the production DRAW_TEXTURE function with host resource interfaces.

GPU allocation/recording is replaced; rectangle math, resource selection and
cycle attachment are compiled from the actual production body, unchanged.
"""
import pathlib
import sys
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from gpu_capture_fixture import strip_capture_blocks
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


with tempfile.TemporaryDirectory(prefix="strato-draw-texture-") as directory:
    directory = pathlib.Path(directory)
    draw = function(strip_capture_blocks((GPU / "interconnect/maxwell_3d/maxwell_3d.cpp").read_text()),
                    "void Maxwell3D::DrawTexture(")
    types = ROOT / "app/src/main/cpp/skyline/soc/gm20b/engines/maxwell/types.h"
    registers = function(types.read_text(), "struct DrawTexture") + ";"
    samplers = (GPU / "interconnect/common/samplers.cpp").read_text()
    filters = "\n".join(function(samplers, "static " + name) for name in (
        "vk::Filter ConvertSamplerFilter(",
        "vk::SamplerMipmapMode ConvertSamplerMipFilter("))
    (directory / "production.inc").write_text(draw)
    (directory / "registers.inc").write_text(registers)
    (directory / "filters.inc").write_text(filters)
    (directory / "sampler_binding.inc").write_text(function(samplers,
        "vk::raii::Sampler *Samplers::GetSampler(InterconnectContext &ctx, u32 samplerIndex, u32 textureIndex)"))
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I" + str(directory), str(ROOT / "tests/gpu_draw_texture/tests.cpp"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True)
