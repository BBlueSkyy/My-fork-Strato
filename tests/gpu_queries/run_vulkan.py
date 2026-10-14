#!/usr/bin/env python3
"""Optional headless Vulkan query/predicate integration; use --required in GPU CI.

Requires g++, Vulkan headers/loader, glslangValidator, spirv-val, a Vulkan ICD,
and VK_LAYER_KHRONOS_validation. Mesa lavapipe works without a display.
VK_ICD_FILENAMES and VK_LAYER_PATH can select non-system installations.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name("vulkan.cpp")
FLAGS = {
    "eAllCommands": "VK_PIPELINE_STAGE_ALL_COMMANDS_BIT",
    "eMemoryRead": "VK_ACCESS_MEMORY_READ_BIT",
    "eMemoryWrite": "VK_ACCESS_MEMORY_WRITE_BIT",
    "eTransfer": "VK_PIPELINE_STAGE_TRANSFER_BIT",
    "eComputeShader": "VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT",
    "eShaderRead": "VK_ACCESS_SHADER_READ_BIT",
    "eShaderWrite": "VK_ACCESS_SHADER_WRITE_BIT",
    "eTransferRead": "VK_ACCESS_TRANSFER_READ_BIT",
    "eTransferWrite": "VK_ACCESS_TRANSFER_WRITE_BIT",
}

def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def barrier(text):
    match = re.search(
        r"pipelineBarrier\(\s*(.*?),\s*(.*?),\s*\{\},\s*vk::MemoryBarrier\{"
        r".*?\.srcAccessMask\s*=\s*(.*?),.*?\.dstAccessMask\s*=\s*(.*?),",
        text, re.S)
    if not match:
        raise RuntimeError("production barrier not found")
    result = []
    for expression in match.groups():
        result.append(" | ".join(FLAGS[flag] for flag in re.findall(
            r"vk::(?:PipelineStageFlagBits|AccessFlagBits)::(e\w+)", expression)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--required", action="store_true", help="fail when Vulkan tooling is unavailable")
    args = parser.parse_args()
    missing = [x for x in ("g++", "glslangValidator", "spirv-val") if not shutil.which(x)]
    if missing:
        print("SKIP Vulkan integration: missing " + ", ".join(missing))
        return 1 if args.required else 0
    gpu = ROOT / "app/src/main/cpp/skyline/gpu"
    helpers = (gpu / "shaders/helper_shaders.cpp").read_text()
    queries = (gpu / "interconnect/maxwell_3d/queries.cpp").read_text()
    barriers = {
        "RESOLVE": function(helpers, "void QueryResolveHelperShader::Resolve("),
        "PREPARE": function(helpers, "void QueryResolveHelperShader::PrepareForTransfer("),
        "RESET": function(queries, "void Queries::Counter::ScheduleResolve("),
    }
    with tempfile.TemporaryDirectory(prefix="strato-vulkan-queries-") as directory:
        tmp = Path(directory)
        queries_header = (gpu / "interconnect/maxwell_3d/queries.h").read_text()
        pool_size = re.search(r"QueryPoolSize\{(0x[0-9a-fA-F]+|[0-9]+)\}", queries_header)[1]
        header = [f"#define QUERY_POOL_SIZE ({pool_size}u)"]
        for prefix, body in barriers.items():
            for suffix, expression in zip(("SRC_STAGE", "DST_STAGE", "SRC_ACCESS", "DST_ACCESS"), barrier(body)):
                header.append(f"#define {prefix}_{suffix} ({expression})")
        report = queries[queries.index("vk::BufferMemoryBarrier reportBarrier{"):]
        access = re.search(r"\.srcAccessMask\s*=\s*(.*?),\s*\.dstAccessMask\s*=\s*(.*?),", report, re.S)
        after_src = re.search(r"reportBarrier.srcAccessMask\s*=\s*(.*?);", report, re.S)
        after_dst = re.search(r"reportBarrier.dstAccessMask\s*=\s*(.*?);", report, re.S)
        for name, expression in zip(("REPORT_BEFORE_SRC_ACCESS", "REPORT_BEFORE_DST_ACCESS", "REPORT_AFTER_SRC_ACCESS", "REPORT_AFTER_DST_ACCESS"), (*access.groups(), after_src[1], after_dst[1])):
            value = " | ".join(FLAGS[x] for x in re.findall(r"vk::AccessFlagBits::(e\w+)", expression))
            header.append(f"#define {name} ({value})")
        report_stages = re.findall(r"commandBuffer.pipelineBarrier\(vk::PipelineStageFlagBits::(e\w+),\s*vk::PipelineStageFlagBits::(e\w+),\s*\{\}, \{\}, reportBarrier", report)
        if len(report_stages) != 2:
            raise RuntimeError("report barrier stage layout changed")
        for prefix, stages in zip(("REPORT_BEFORE", "REPORT_AFTER"), report_stages):
            for suffix, stage in zip(("SRC_STAGE", "DST_STAGE"), stages):
                header.append(f"#define {prefix}_{suffix} ({FLAGS[stage]})")
        (tmp / "production_barriers.inc").write_text("\n".join(header) + "\n")
        probe = tmp / "probe.cpp"
        probe.write_text("#include <vulkan/vulkan.h>\nint main(){uint32_t v;return vkEnumerateInstanceVersion(&v);}\n")
        compiler = ["g++", "-std=c++20", "-O1", "-g", "-I" + str(tmp)]
        tested = subprocess.run(compiler + [str(probe), "-lvulkan", "-o", str(tmp / "probe")], capture_output=True, text=True)
        if tested.returncode:
            print("SKIP Vulkan integration: Vulkan headers/loader unavailable")
            if args.required:
                print(tested.stderr)
            return 1 if args.required else 0
        for shader in ("query_resolve", "conditional_compare"):
            output = tmp / (shader + ".spv")
            subprocess.run(["glslangValidator", "-V", str(ROOT / "app/src/main/shaders" / (shader + ".comp")), "-o", str(output)], check=True)
            subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(output)], check=True)
        triangles = {"vert": "#version 450\nvec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));\nvoid main(){gl_Position=vec4(p[gl_VertexIndex],0,1);}\n", "frag": "#version 450\nvoid main(){}\n"}
        for stage, text in triangles.items():
            path = tmp / ("triangle." + stage)
            path.write_text(text)
            subprocess.run(["glslangValidator", "-V", str(path), "-o", str(path) + ".spv"], check=True)
        executable = tmp / "vulkan-queries"
        subprocess.run(compiler + [str(SOURCE), "-lvulkan", "-o", str(executable)], check=True)
        env = os.environ.copy()
        env.setdefault("XDG_RUNTIME_DIR", str(tmp))
        for conditional in (False, True):
            command = [str(executable)] + (["conditional"] if conditional else [])
            result = subprocess.run(command, cwd=tmp, env=env, text=True, capture_output=True)
            print(result.stdout, end="")
            if result.returncode:
                if "unavailable Vulkan runtime" in result.stderr and not args.required:
                    print("SKIP Vulkan integration: validation layer or Vulkan ICD unavailable")
                    return 0
                print(result.stderr, end="")
                return result.returncode
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
