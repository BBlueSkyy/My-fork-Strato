#!/usr/bin/env python3
"""Run the production present-copy body with Vulkan-Hpp values and host facades.

No Vulkan driver is used. Only engine scheduling/resource ownership and command
dispatch are replaced; layouts, barriers and copy descriptors use pinned Hpp.
GCC rejects the existing out-of-order `.image` designated initializers, so the
extracted body moves that field to its declaration order without changing values.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def ordered_image_barrier(match):
    body = match[1]
    field = re.search(r"\s*\.image = [^,]+,", body)
    if field is None:
        raise RuntimeError("ImageMemoryBarrier without image initializer")
    image = field[0]
    body = body[:field.start()] + body[field.end():]
    position = body.index(".subresourceRange")
    return "vk::ImageMemoryBarrier{" + body[:position] + image.strip() + "\n                        " + body[position:] + "}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparability-ref", help="Read only the comparable declaration from this git revision (regression demonstration)")
    args = parser.parse_args()
    hpp = ROOT / "app/libraries/vkhpp"
    headers = hpp / "Vulkan-Headers/include"
    if not (hpp / "vulkan/vulkan.hpp").exists() or not (headers / "vulkan/vulkan.h").exists():
        raise SystemExit("Initialize vkhpp and its nested Vulkan-Headers submodule before running this test")
    source = (GPU / "texture/texture.cpp").read_text()
    body = function(source, "void Texture::CopyFrom(")
    body = re.sub(r"vk::ImageMemoryBarrier\{(.*?)\}", ordered_image_barrier, body, flags=re.S)
    metadata = (GPU / "texture/texture.h").read_text()
    presentation = (GPU / "presentation_engine.cpp").read_text()
    copy_selection = re.search(r"\.usesImageCopy\s*=\s*([^,]+),", presentation)[1]
    comparable_source = presentation
    if args.comparability_ref:
        comparable_source = subprocess.check_output(
            ["git", "show", args.comparability_ref + ":app/src/main/cpp/skyline/gpu/presentation_engine.cpp"],
            cwd=ROOT, text=True)
    comparable = re.search(r"const bool comparable\{[^;]+;", comparable_source)[0]
    with tempfile.TemporaryDirectory(prefix="strato-present-copy-") as directory:
        build = Path(directory)
        (build / "present_copy.inc").write_text(body + "\n")
        (build / "present_readback.inc").write_text(function(metadata, "struct PresentPixelReadback") + ";\n")
        (build / "uses_image_copy.inc").write_text(copy_selection + "\n")
        (build / "raw_comparable.inc").write_text(comparable + "\n")
        flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-Wno-missing-field-initializers", "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS",
                 "-DVULKAN_HPP_NO_SPACESHIP_OPERATOR", "-DVULKAN_HPP_NO_SETTERS",
                 "-DVULKAN_HPP_NO_SMART_HANDLE", "-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1",
                 "-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0"]
        if os.environ.get("SANITIZE", "1") != "0":
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run([os.environ.get("CXX", "g++"), *flags, "-I" + str(build),
                        "-I" + str(hpp), "-I" + str(headers),
                        str(Path(__file__).with_name("botw_present_copy_test.cpp")),
                        "-o", str(build / "tests")], check=True)
        subprocess.run([str(build / "tests")], check=True,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})


if __name__ == "__main__":
    main()
