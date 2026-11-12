#!/usr/bin/env python3
"""Exercise production descriptor-reuse selection and texture read tracking.

No Vulkan device is needed. Set SAMPLED_BARRIERS_BASELINE_REF for the negative
control; the same assertions must fail against the unmodified PrepareDraw.
"""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def source(path):
    ref = os.environ.get("SAMPLED_BARRIERS_BASELINE_REF")
    if ref:
        return subprocess.check_output(["git", "show", f"{ref}:{path.relative_to(ROOT)}"], cwd=ROOT, text=True)
    return path.read_text()


with tempfile.TemporaryDirectory(prefix="strato-sampled-barriers-") as temp:
    build = pathlib.Path(temp)
    draw = source(GPU / "interconnect/maxwell_3d/maxwell_3d.cpp")
    draw = function(draw, "void Maxwell3D::PrepareDraw(")
    start = draw.index("\n", draw.index("activeDescriptorSetSampledImages.resize"))
    end = draw.index("         activeDescriptorPipeline = pipeline;")
    (build / "reuse.inc").write_text(draw[start:end] + "\n(void)descUpdateInfo;\n")
    pipeline = source(GPU / "interconnect/maxwell_3d/pipeline_manager.cpp")
    signature = "void Pipeline::PopulateSampledImageReadBarriers("
    (build / "pipeline.inc").write_text(function(pipeline, signature) if signature in pipeline else "")
    texture = (GPU / "texture/texture.cpp").read_text()
    (build / "texture.inc").write_text("\n".join(function(texture, sig) for sig in (
        "void Texture::UpdateRenderPassUsage(", "void Texture::PopulateReadBarrier(")))
    includes = [build, ROOT / "app/libraries/vkhpp",
                pathlib.Path(os.environ.get("VULKAN_HPP_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp"))),
                pathlib.Path(os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")))]
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                    *["-I" + str(p) for p in includes], str(ROOT / "tests/gpu_sampled_barriers/tests.cpp"),
                    "-o", str(build / "tests")], check=True)
    subprocess.run([str(build / "tests")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
