#!/usr/bin/env python3
"""Exercise exact executor publication bodies against deterministic resource facades.

The Vulkan mixed-resource byte transport gate is gpu_backing_promotion. This
gate isolates preparation, cached replay, node allocation and publication.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(source, signature):
    begin = source.index(signature)
    cursor = source.index("{", begin) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[begin:cursor]


with tempfile.TemporaryDirectory(prefix="strato-region-executor-") as temporary:
    output = Path(temporary)
    executor = (GPU / "interconnect/command_executor.cpp").read_text()
    prepare = function(executor, "void CommandExecutor::PrepareBufferWrite(")
    if os.getenv("EXECUTOR_EAGER_CLAIM"):
        prepare = prepare.replace("view.GetBuffer()->PrepareGpuWrite(usageTracker);",
                                  "view.GetBuffer()->CommitGpuWrite(usageTracker, view.GetOffset(), view.size);")
    bodies = [function(executor, signature) for signature in (
        "void CommandExecutor::TrackBufferRegionUse(",
        "bool CommandExecutor::AttachBuffer(",
        "CommandExecutor::RegionPublication CommandExecutor::AcquireRegionPublication(",
        "void CommandExecutor::AddOutsideRpCommand(",
        "void CommandExecutor::AddPostRpCommand(",
    )]
    (output / "executor.inc").write_text(prepare + "\n" + "\n".join(bodies))
    pipeline = (GPU / "interconnect/maxwell_3d/pipeline_manager.cpp").read_text()
    (output / "cached.inc").write_text(function(pipeline, "void Pipeline::PreflightDescriptorBuffers("))
    regions = (GPU / "buffer_regions.inc").read_text()
    (output / "validate.inc").write_text(function(regions, "void Buffer::ValidateRegionWrites("))
    command = [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-fsanitize=" + os.getenv("SANITIZE", "undefined"),
               "-fno-sanitize-recover=all", "-pthread", "-I" + str(output),
               "-I" + str(ROOT / "app/src/main/cpp/skyline"),
               "-I" + os.getenv("BOOST_INCLUDE_DIR", "/usr/include"),
               "-I" + os.getenv("VULKAN_HPP_INCLUDE_DIR", "/usr/include"),
               str(Path(__file__).with_name("tests.cpp")), "-o", str(output / "tests")]
    subprocess.run(command + shlex.split(os.getenv("CXXFLAGS", "")), check=True)
    subprocess.run([str(output / "tests")], check=True)
    nodes = (GPU / "interconnect/command_nodes.cpp").read_text()
    if os.getenv("EXECUTOR_EAGER_LOADOP"):
        nodes = nodes.replace("clearValues.resize(attachmentIndex + 1);\n            attachmentDescription.loadOp = vk::AttachmentLoadOp::eClear;",
                              "attachmentDescription.loadOp = vk::AttachmentLoadOp::eClear;\n            clearValues.resize(attachmentIndex + 1);")
    node_header = (GPU / "interconnect/command_nodes.h").read_text().replace("#include <gpu.h>", "")
    # The clear storage allocator exposes its existing allocation-failure seam;
    # all production method bodies and the attachment bookkeeping stay intact.
    node_header = node_header.replace("std::vector<vk::ClearValue> clearValues;",
                                      "std::vector<vk::ClearValue, NodeAllocator<vk::ClearValue>> clearValues;")
    (output / "clear_nodes.h").write_text(node_header)
    (output / "clear_nodes.inc").write_text(next(line for line in nodes.splitlines() if "RenderPassNode::RenderPassNode(" in line) + "\n" + "\n".join(function(nodes, signature) for signature in (
        "u32 RenderPassNode::AddAttachment(",
        "void RenderPassNode::AddSubpass(",
        "void RenderPassNode::UpdateDependency(",
        "bool RenderPassNode::ClearColorAttachment(",
        "bool RenderPassNode::ClearDepthStencilAttachment(",
    )))
    clear_bodies = "\n".join(function(executor, signature) for signature in (
        "static bool ViewsEqual(",
        "CommandExecutor::RegionPublication CommandExecutor::AcquireRegionPublication(",
        "bool CommandExecutor::CreateRenderPassWithSubpass(",
        "void CommandExecutor::AddSubpass(",
        "void CommandExecutor::AddClearSubpass(",
        "void CommandExecutor::AddClearColorSubpass(",
        "void CommandExecutor::AddClearDepthStencilSubpass(",
    ))
    if os.getenv("EXECUTOR_EAGER_CLEAR"):
        clear_bodies = clear_bodies.replace("{}, true)};", "{}, false)};")
        clear_bodies = clear_bodies.replace("            attachment->texture->UpdateRenderPassUsage(renderPassIndex, texture::RenderPassUsage::RenderTarget);", "")
    (output / "clear_executor.inc").write_text(clear_bodies)
    clear_command = [str(Path(__file__).with_name("clears.cpp")) if part == str(Path(__file__).with_name("tests.cpp"))
                     else str(output / "clears") if part == str(output / "tests") else part for part in command]
    subprocess.run(clear_command + shlex.split(os.getenv("CXXFLAGS", "")), check=True)
    subprocess.run([str(output / "clears")], check=True)

