#!/usr/bin/env python3
"""Exercise the production render-pass key/hash/equality without Android/GPU.

--benchmark reports a synthetic cache workload, never device performance.
RENDERPASS_SOURCE optionally selects an unmodified baseline .cpp for comparison.
"""
import argparse
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


parser = argparse.ArgumentParser()
parser.add_argument("--benchmark", action="store_true")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="strato-renderpass-cache-") as directory:
    directory = pathlib.Path(directory)
    header = (GPU / "cache/renderpass_cache.h").read_text()
    (directory / "class.inc").write_text(function(header, "class RenderPassCache") + ";")
    source = pathlib.Path(os.environ.get("RENDERPASS_SOURCE", str(GPU / "cache/renderpass_cache.cpp"))).read_text()
    signatures = (
        "size_t RenderPassCache::RenderPassHash::operator()(const RenderPassMetadata &",
        "size_t RenderPassCache::RenderPassHash::operator()(const vk::RenderPassCreateInfo &",
        "bool RenderPassCache::RenderPassEqual::operator()(const RenderPassMetadata &lhs, const RenderPassMetadata &",
        "bool RenderPassCache::RenderPassEqual::operator()(const RenderPassMetadata &lhs, const vk::RenderPassCreateInfo &",
    )
    (directory / "production.inc").write_text(
        "#define VEC_CPY(pointer, size) description.pointer, description.pointer + description.size\n"
        "#define HASH(x) boost::hash_combine(hash, x)\n" +
        source[source.index("RenderPassCache::SubpassDescription::SubpassDescription("):source.index("    #undef VEC_CPY")] +
        source[source.index("RenderPassCache::RenderPassMetadata::RenderPassMetadata("):source.index("    #define HASH")] +
        "\n".join(function(source, signature) for signature in signatures) +
        "\n#undef HASH\n#undef VEC_CPY\n")
    includes = [directory, ROOT / "app/libraries/vkhpp",
                pathlib.Path(os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")))]
    includes += list((ROOT / "app/libraries/boost/libs").glob("*/include"))
    flags = ["-O3", "-DNDEBUG"] if args.benchmark else ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", *flags,
                    *["-I" + str(path) for path in includes], str(ROOT / "tests/gpu_renderpass_cache/tests.cpp"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests"), *(["--benchmark"] if args.benchmark else [])], check=True)
