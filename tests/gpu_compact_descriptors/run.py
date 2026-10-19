#!/usr/bin/env python3
"""Compile production compact descriptors against the actual compiler API."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HOST = Path(os.environ.get("HOST_DEPS", ROOT.parent / "shader-host-deps"))
shader = Path(os.environ.get("SHADER_COMPILER_ROOT", ROOT / "app/libraries/shader-compiler"))
if not (shader / "shader_info.h").exists():
    shader = ROOT.parent / "shader-compiler"
header = (ROOT / "app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.h").read_text()

def definition(name):
    start = header.index("struct " + name + " {")
    end = header.index("{", start) + 1
    depth = 1
    while depth:
        depth += (header[end] == "{") - (header[end] == "}")
        end += 1
    return header[start:end] + ";\n"

with tempfile.TemporaryDirectory(prefix="strato-compact-descriptors-") as temporary:
    temporary = Path(temporary)
    (temporary / "shader_compiler").symlink_to(shader.resolve(), target_is_directory=True)
    names = ("UniformBufferDesc", "StorageBufferDesc", "UniformTexelBufferDesc",
             "StorageTexelBufferDesc", "CombinedImageSamplerDesc", "StorageImageDesc")
    (temporary / "compact.inc").write_text("".join(definition(name) for name in names))
    includes = [temporary, HOST / "boost-root/usr/include", HOST / "fmt/include"]
    if "BOOST_INCLUDE" in os.environ:
        includes.insert(1, Path(os.environ["BOOST_INCLUDE"]))
    if "FMT_INCLUDE" in os.environ:
        includes.insert(1, Path(os.environ["FMT_INCLUDE"]))
    command = shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-fno-sanitize-recover=all",
        *("-I" + str(path) for path in includes),
        str(Path(__file__).with_name("tests.cpp")), "-o", str(temporary / "tests")]
    subprocess.run(command, check=True)
    subprocess.run([str(temporary / "tests")], check=True)
