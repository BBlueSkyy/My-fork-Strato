#!/usr/bin/env python3
"""Compile the production render-pass clear method against real Vulkan types."""
import os
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--ref', help='Compile the clear methods from this revision as a negative control')
args = parser.parse_args()
path = 'app/src/main/cpp/skyline/gpu/interconnect/command_nodes.cpp'
source = (subprocess.check_output(['git', 'show', args.ref + ':' + path], cwd=ROOT, text=True)
          if args.ref else (ROOT / path).read_text())
methods = []
for name in ("ClearColorAttachment", "ClearDepthStencilAttachment"):
    start = source.index(f"bool RenderPassNode::{name}(")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    methods.append(source[start:end])

with tempfile.TemporaryDirectory(prefix="strato-depth-stencil-clear-") as directory:
    directory = pathlib.Path(directory)
    (directory / "production.inc").write_text("\n".join(methods))
    includes = [ROOT / "app/libraries/vkhpp", pathlib.Path(os.environ.get(
        "VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include"))), directory]
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    *["-I" + str(path) for path in includes],
                    str(ROOT / "tests/gpu_depth_stencil_clear/tests.cpp"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True)
