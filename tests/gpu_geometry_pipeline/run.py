#!/usr/bin/env python3
"""Compile actual pipeline runtime policy with host state interfaces under UBSan."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "app/src/main/cpp/skyline/gpu/interconnect/maxwell_3d/pipeline_manager.cpp"

def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

with tempfile.TemporaryDirectory(prefix="strato-geometry-runtime-") as directory:
    directory = pathlib.Path(directory)
    (directory / "shader_compiler").symlink_to(ROOT / "app/libraries/shader-compiler", target_is_directory=True)
    source = SOURCE.read_text()
    names = ["static Shader::OutputTopology ConvertShaderOutputTopology(",
             "static Shader::RuntimeInfo MakeRuntimeInfo("]
    if "static bool HasConventionalGeometryStage(" in source:
        names.insert(1, "static bool HasConventionalGeometryStage(")
    (directory / "production.inc").write_text("\n".join(function(source, name) for name in names))
    types = (ROOT / "app/src/main/cpp/skyline/soc/gm20b/engines/maxwell/types.h").read_text()
    (directory / "topology.inc").write_text(function(types, "enum class DrawTopology") + ";")
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I" + str(directory), "-I" + str(ROOT / "app/libraries"),
                    "-I" + str(ROOT / "app/libraries/fmt/include"),
                    str(ROOT / "tests/gpu_geometry_pipeline/tests.cpp"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True)
