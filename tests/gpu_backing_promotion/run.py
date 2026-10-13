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
    baseline = os.environ.get("TEXMAN_BASELINE_VIEWS_REF")
    view_source = subprocess.check_output(["git", "show", baseline + ":app/src/main/cpp/skyline/gpu/texture/texture.cpp"],
        cwd=ROOT, text=True) if baseline else source
    # Optional negative control: replace only the two view APIs with their
    # actual baseline bodies. The current bounds test must then fail.
    (directory / "production.inc").write_text("\n".join(function(
        view_source if signature in ("std::shared_ptr<TextureView> Texture::GetView(", "vk::ImageView TextureView::GetView(") else source,
        signature) for signature in (
        "bool Texture::ViewFormatAdmitted(",
        "bool Texture::ViewRangeFits(",
        "bool Texture::CanGrowMipLevels(",
        "std::shared_ptr<void> Texture::PinGuestMemoryForMipGrowth(",
        "void Texture::FreeGuest(",
        "void Texture::PromoteBackingForView(",
        "std::shared_ptr<TextureView> Texture::GetView(",
        "vk::ImageUsageFlags TextureView::GetUsage(",
        "vk::ImageView TextureView::GetView(",
        "void Texture::SwapBacking(")))
    layout = (GPU / "texture/layout.cpp").read_text()
    header = (GPU / "texture/texture.h").read_text()
    (directory / "layout.inc").write_text(function(header, "struct MipLevelLayout") + ";\n" +
        "\n".join(function(layout, signature) for signature in (
            "template<typename Type>\n    constexpr Type CalculateBlockGobs(",
            "std::vector<MipLevelLayout> GetBlockLinearMipLayout(",
            "template<bool BlockLinearToPitch>\n    void CopyBlockLinearInternal(",
            "void CopyBlockLinearToLinear(Dimensions dimensions",
            "void CopyLinearToBlockLinear(Dimensions dimensions")))
    (directory / "sizes.inc").write_text("\n".join(function(source, signature) for signature in (
        "size_t CalculateLevelStride(", "size_t CalculateTargetLevelStride(")))
    cache = (GPU / "cache/framebuffer_cache.cpp").read_text()
    (directory / "cache.inc").write_text(function(cache,
        "std::vector<vk::raii::Framebuffer> FramebufferCache::RetireViews("))
    nodes = (GPU / "interconnect/command_nodes.cpp").read_text()
    (directory / "attachment.inc").write_text(function(nodes,
        "attachmentInfo.push_back(vk::FramebufferAttachmentImageInfo") + ");")
    # Compile the exact full-match dimension predicate from 2c293491 too.
    manager = (GPU / "texture_manager.cpp").read_text()
    start = manager.index("(matchGuestTexture.dimensions.width")
    end = manager.index("&& matchGuestTexture.GetImageType()", start)
    (directory / "extent.inc").write_text("return " + manager[start:end].strip() + ";")
    start = manager.index("if (matchGuestTexture.format->IsCompatible")
    end = manager.index(") {", start)
    (directory / "match.inc").write_text("return " + manager[start + 3:end + 1] + ";")
    (directory / "selection.inc").write_text(function(manager, "else if (fullMatch)"))
    (directory / "request_range.inc").write_text(function(manager, "const vk::ImageSubresourceRange requestedRange") + ";")
    (directory / "request_validation.inc").write_text(function(manager, "if (!texture::SubresourceRangeFits("))
    executor = (GPU / "interconnect/command_executor.cpp").read_text()
    (directory / "retry.inc").write_text(function(executor,
        "void CommandExecutor::HandleTextureBackingPromotion("))
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", "-Wno-unknown-pragmas", "-fsanitize=undefined",
                    "-fno-sanitize-recover=all", "-pthread", "-I" + str(directory),
                    "-I" + str(ROOT / "app/src/main/cpp/skyline"),
                    "-I" + os.environ.get("VULKAN_HPP_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp")),
                    "-I" + os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")),
                    str(ROOT / "tests/gpu_backing_promotion/tests.cpp"),
                    os.environ.get("VULKAN_LOADER_LIBRARY", "-lvulkan"),
                    "-o", str(directory / "tests")], check=True)
    subprocess.run([str(directory / "tests")], check=True)
