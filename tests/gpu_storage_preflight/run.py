#!/usr/bin/env python3
"""Compile production image-binding resolution and descriptor allocation tails."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu/interconnect"
HERE = Path(__file__).resolve().parent


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


common = (GPU / "common/pipeline.inc").read_text()
deferred = "DeferredStorageImageWrites" in common
with tempfile.TemporaryDirectory(prefix="strato-storage-preflight-") as directory:
    directory = Path(directory)
    code = function(common, "static std::pair<vk::DescriptorImageInfo, TextureView *> GetImageBinding(")
    executor = (GPU / "command_executor.cpp").read_text()
    (directory / "commit.inc").write_text(function(executor, "void CommandExecutor::CommitTextureWrites("))
    (directory / "binding.inc").write_text(code)
    paths = [("maxwell_3d/pipeline_manager.cpp", "DescriptorUpdateInfo *Pipeline::SyncDescriptors("),
             ("maxwell_3d/pipeline_manager.cpp", "DescriptorUpdateInfo *Pipeline::SyncDescriptorsQuickBind("),
             ("kepler_compute/pipeline_manager.cpp", "DescriptorUpdateInfo *Pipeline::SyncDescriptors(")]
    for index, (path, signature) in enumerate(paths):
        source = function((GPU / path).read_text(), signature)
        marker = "auto *updateInfo = ctx.executor.allocator" if "auto *updateInfo = ctx.executor.allocator" in source else "return ctx.executor.allocator"
        start = source.index(marker)
        tail = source[start:source.rfind("}")]
        (directory / f"tail{index}.inc").write_text(tail)
    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I" + str(directory),
               "-DHAS_STORAGE_PREFLIGHT=" + str(int(deferred)), str(HERE / "tests.cpp"), "-o", str(directory / "tests")]
    subprocess.run(command, check=True)
    subprocess.run([str(directory / "tests")], check=True, timeout=10)
