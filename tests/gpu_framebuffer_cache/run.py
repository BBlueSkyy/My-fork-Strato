#!/usr/bin/env python3
"""Compile production framebuffer keys/hash/equality without Android or a GPU."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


with tempfile.TemporaryDirectory(prefix="strato-framebuffer-cache-") as directory:
    directory = pathlib.Path(directory)
    header = (GPU / "cache/framebuffer_cache.h").read_text()
    # Expose key types only in the host test; their definition stays unmodified.
    (directory / "class.inc").write_text(function(header, "class FramebufferCache") + ";")
    source = (GPU / "cache/framebuffer_cache.cpp").read_text()
    (directory / "production.inc").write_text("#define HASH(x) boost::hash_combine(hash, x)\n" +
        "\n".join(function(source, signature) for signature in (
            "FramebufferCache::FramebufferImagelessAttachment::FramebufferImagelessAttachment(",
            "FramebufferCache::FramebufferCacheKey::FramebufferCacheKey(",
            "size_t FramebufferCache::FramebufferHash::operator()(const FramebufferCacheKey &",
            "size_t FramebufferCache::FramebufferHash::operator()(const FramebufferCreateInfo &",
            "bool FramebufferCache::FramebufferEqual::operator()(const FramebufferCacheKey &lhs, const FramebufferCacheKey &",
            "bool FramebufferCache::FramebufferEqual::operator()(const FramebufferCacheKey &lhs, const FramebufferCreateInfo &")) +
        "\n#undef HASH\n")
    includes = [directory, ROOT / "app/libraries/vkhpp",
                pathlib.Path(os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")))]
    includes += list((ROOT / "app/libraries/boost/libs").glob("*/include"))
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    *["-I" + str(path) for path in includes], str(ROOT / "tests/gpu_framebuffer_cache/tests.cpp"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True)
