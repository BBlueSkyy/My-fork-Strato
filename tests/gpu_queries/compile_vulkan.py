#!/usr/bin/env python3
"""Syntax-check unchanged query and helper code against pinned Vulkan-Hpp.

This replaces Android/engine interfaces with declarations, keeping real Vulkan
RAII classes, constructors, descriptors, handle conversions and command overloads.
It complements host tests; it is not a full Android build.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--required", action="store_true")
    args = parser.parse_args()
    hpp = ROOT / "app/libraries/vkhpp"
    headers = hpp / "Vulkan-Headers/include"
    if not shutil.which("g++") or not (hpp / "vulkan/vulkan_raii.hpp").exists() or not (headers / "vulkan/vulkan.h").exists():
        print("SKIP Vulkan compile smoke: initialize vkhpp and nested Vulkan-Headers submodules; g++ is required")
        return 1 if args.required else 0
    with tempfile.TemporaryDirectory(prefix="strato-query-vkhpp-") as directory:
        tmp = Path(directory)
        header = (GPU / "interconnect/maxwell_3d/queries.h").read_text()
        source = (GPU / "interconnect/maxwell_3d/queries.cpp").read_text()
        (tmp / "queries_header.inc").write_text(header[header.index("namespace skyline"):])
        (tmp / "queries_source.inc").write_text(source[source.index("namespace skyline"):])
        buffer_header = (GPU / "buffer.h").read_text()
        read = re.search(r"void Read\(bool isFirstUsage,[^;]*readOffset\) const;", buffer_header)[0]
        (tmp / "buffer_read.inc").write_text(read)
        maxwell_header = (GPU / "interconnect/maxwell_3d/maxwell_3d.h").read_text()
        nested = "\n".join(function(maxwell_header, signature) + ";" for signature in (
            "enum class RenderConditionMode", "struct RenderConditionKey", "struct RenderConditionResources",
            "struct RenderConditionInput", "struct RenderConditionBinding"))
        fields = maxwell_header[maxwell_header.index("        CachedMappedBufferView renderConditionLhsView"):maxwell_header.index("        void RefreshRenderCondition();")]
        (tmp / "conditional_header.inc").write_text("namespace skyline::gpu::interconnect::maxwell3d { class Maxwell3D {\n" + nested + "\nInterconnectContext ctx; Queries queries;\n" + fields + "\nvoid FlushQueries(); void ClearRenderCondition(); void RefreshRenderCondition();\nbool CheckRenderCondition(soc::gm20b::IOVA, std::optional<soc::gm20b::IOVA>, RenderConditionMode);\nbool SetRenderCondition(soc::gm20b::IOVA, std::optional<soc::gm20b::IOVA>, RenderConditionMode);\n}; }\n")
        maxwell_source = (GPU / "interconnect/maxwell_3d/maxwell_3d.cpp").read_text()
        conditional = "\n".join(function(maxwell_source, signature) for signature in (
            "void Maxwell3D::FlushQueries(",
            "void Maxwell3D::ClearRenderCondition(", "bool Maxwell3D::CheckRenderCondition(",
            "void Maxwell3D::RefreshRenderCondition(", "bool Maxwell3D::SetRenderCondition("))
        resources = maxwell_source[maxwell_source.index("    Maxwell3D::RenderConditionResources::RenderConditionResources("):maxwell_source.index("    Maxwell3D::Maxwell3D(")]
        conditional = resources + conditional
        (tmp / "conditional_source.inc").write_text("namespace skyline::gpu::interconnect::maxwell3d {\n" + conditional + "\n}")
        helpers_header = (GPU / "shaders/helper_shaders.h").read_text()
        (tmp / "helpers_header.inc").write_text("\n".join(function(helpers_header, "class " + name) + ";" for name in (
            "QueryResolveHelperShader", "ConditionalCompareHelperShader")))
        helpers = (GPU / "shaders/helper_shaders.cpp").read_text()
        begin = helpers.index("    static vk::raii::DescriptorSetLayout CreateQueryResolveDescriptorSetLayout(")
        end = helpers.index("    HelperShaders::HelperShaders(", begin)
        (tmp / "helpers_source.inc").write_text(helpers[begin:end])
        command = ["g++", "-std=c++20", "-fsyntax-only", "-Werror", "-Wno-unused-function",
                   "-DVULKAN_HPP_NO_SPACESHIP_OPERATOR", "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS",
                   "-DVULKAN_HPP_NO_SETTERS", "-DVULKAN_HPP_NO_SMART_HANDLE",
                   "-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1", "-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0",
                   "-I" + str(tmp), "-I" + str(hpp), "-I" + str(headers),
                   str(Path(__file__).with_name("vulkan_compile.cpp"))]
        subprocess.run(command, check=True)
        print("PASS exact production Queries declarations/implementations both helper pipelines, and Maxwell conditional preparation/fallback with pinned Vulkan-Hpp")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
