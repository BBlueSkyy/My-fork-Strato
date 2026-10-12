#!/usr/bin/env python3
"""Compile actual promotion/view/cache methods against real Vulkan resources.

Only allocator, scheduler and fence interfaces are host facades. Allocation,
image views, copy commands and fences use the Vulkan ICD and validation layer.
"""
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


with tempfile.TemporaryDirectory(prefix="strato-backing-promotion-") as directory:
    directory = pathlib.Path(directory)
    source = (GPU / "texture/texture.cpp").read_text()
    (directory / "production.inc").write_text("\n".join(function(source, signature) for signature in (
        "bool Texture::ViewFormatAdmitted(",
        "void Texture::PromoteBackingForView(",
        "std::shared_ptr<TextureView> Texture::GetView(",
        "vk::ImageUsageFlags TextureView::GetUsage(",
        "vk::ImageView TextureView::GetView(",
        "void Texture::SwapBacking(")))
    cache = (GPU / "cache/framebuffer_cache.cpp").read_text()
    (directory / "cache.inc").write_text(function(cache,
        "std::vector<vk::raii::Framebuffer> FramebufferCache::RetireViews("))
    nodes = (GPU / "interconnect/command_nodes.cpp").read_text()
    (directory / "attachment.inc").write_text(function(nodes,
        "attachmentInfo.push_back(vk::FramebufferAttachmentImageInfo") + ");")
    # Compile the exact full-match dimension predicate from 2c293491 too.
    manager = (GPU / "texture_manager.cpp").read_text()
    start = manager.index("(((matchGuestTexture.dimensions.width")
    end = manager.index("&& matchGuestTexture.tileConfig", start)
    (directory / "extent.inc").write_text("return " + manager[start:end].strip() + ";")
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", "-fsanitize=undefined",
                    "-fno-sanitize-recover=all", "-pthread", "-I" + str(directory),
                    "-I" + str(ROOT / "app/src/main/cpp/skyline"),
                    "-I" + os.environ.get("VULKAN_HPP_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp")),
                    "-I" + os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")),
                    str(ROOT / "tests/gpu_backing_promotion/tests.cpp"),
                    os.environ.get("VULKAN_LOADER_LIBRARY", "-lvulkan"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True)
