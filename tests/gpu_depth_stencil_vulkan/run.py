#!/usr/bin/env python3
"""Headless integration of production depth/stencil packing and Vulkan transfers."""
import os
from pathlib import Path
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


with tempfile.TemporaryDirectory(prefix="strato-depth-stencil-") as directory:
    temporary = Path(directory)
    source = (GPU / "texture/texture.cpp").read_text()
    baseline = os.environ.get("DEPTH_STENCIL_BASELINE_COPIES_REF")
    copy_source = subprocess.check_output(["git", "show", baseline + ":app/src/main/cpp/skyline/gpu/texture/texture.cpp"],
        cwd=ROOT, text=True) if baseline else source
    (temporary / "production.inc").write_text("\n".join(function(
        copy_source if "GetBufferImageCopies" in signature else source, signature) for signature in (
        "boost::container::small_vector<vk::BufferImageCopy, 10> Texture::GetBufferImageCopies(",
        "void Texture::CopyFromStagingBuffer(",
        "void Texture::CopyIntoStagingBuffer(")))
    executable = temporary / "tests"
    shader = temporary / "sample.comp"
    shader.write_text("""#version 450
layout(local_size_x=1) in;
layout(binding=0) uniform sampler2D depthImage;
layout(binding=1,std430) buffer Result { float values[]; } result;
void main() { uint i=gl_GlobalInvocationID.x; result.values[i]=texelFetch(depthImage,ivec2(i%4,i/4),0).r; }
""")
    subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.1", str(shader), "-o", str(temporary / "sample.spv")], check=True)
    subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(temporary / "sample.spv")], check=True)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g",
        "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", "-Wall", "-Wextra", "-Werror",
        "-I" + str(temporary), "-I" + str(GPU.parent),
        "-I" + os.environ.get("VULKAN_HPP_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp")),
        "-I" + os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")),
        str(Path(__file__).with_name("tests.cpp")), os.environ.get("VULKAN_LOADER_LIBRARY", "-lvulkan"),
        "-o", str(executable)], check=True)
    subprocess.run([str(executable)], cwd=temporary, check=True)
