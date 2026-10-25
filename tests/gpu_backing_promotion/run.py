#!/usr/bin/env python3
"""Compile actual promotion/view/cache methods against real Vulkan resources.

Only allocator, scheduler and fence interfaces are host facades. Allocation,
image views, copy commands and fences use the Vulkan ICD and validation layer.
"""
import os
import pathlib
import shlex
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
    # The storage-identity view API from PR #383 adds an optional bool. Match
    # the exact production definition under test, including older baselines.
    view_api = function(view_source, "vk::ImageView TextureView::GetView(").split("{", 1)[0].strip()
    view_api = view_api.replace("TextureView::", "").replace("(bool identityMapping)", "(bool identityMapping = false)")
    (directory / "view_api.inc").write_text(view_api + ";\n")
    # Optional negative control: replace only the two view APIs with their
    # actual baseline bodies. The current bounds test must then fail.
    (directory / "production.inc").write_text("\n".join(function(
        view_source if signature in ("std::shared_ptr<TextureView> Texture::GetView(", "vk::ImageView TextureView::GetView(") else source,
        signature) for signature in (
        "void Texture::unlock(",
        "void Texture::SetupGuestMappings(",
        "bool Texture::CanMaterializeGuestLayout(",
        "bool Texture::IsGuestLayoutReversible(",
        "bool Texture::CanReconcileGuestLayout(",
        "std::shared_ptr<texture::AliasGroup> Texture::RegisterAliases(",
        "void Texture::RequireAliasCoherence(",
        "void Texture::ReconcileAliases(",
        "void Texture::RecordGpuWrite(",
        "void Texture::ValidateAliasWrites(",
        "void Texture::MarkGpuDirty(",
        "void Texture::UpdateRenderPassUsage(",
        "bool Texture::ViewFormatAdmitted(",
        "bool Texture::ViewRangeFits(",
        "bool Texture::CanGrowMipLevels(",
        "std::shared_ptr<void> Texture::PinGuestMemoryForMipGrowth(",
        "std::shared_ptr<void> Texture::PinGuestMemory(",
        "void Texture::FreeGuest(",
        "boost::container::small_vector<vk::BufferImageCopy, 10> Texture::GetBufferImageCopies(",
        "void Texture::CopyFromStagingBuffer(",
        "void Texture::CopyIntoStagingBuffer(",
        "void Texture::CopyToGuest(",
        "std::shared_ptr<memory::StagingBuffer> Texture::SynchronizeHostImpl(",
        "void Texture::SynchronizeHost(",
        "void Texture::SynchronizeHostInline(",
        "void Texture::SynchronizeGuest(",
        "void Texture::PromoteBackingForView(",
        "std::shared_ptr<TextureView> Texture::GetView(",
        "vk::ImageUsageFlags TextureView::GetUsage(",
        "vk::ImageView TextureView::GetView(",
        "void Texture::SwapBacking(")))
    layout = (GPU / "texture/layout.cpp").read_text()
    header = (GPU / "texture/texture.h").read_text()
    (directory / "layout.inc").write_text(function(header, "struct MipLevelLayout") + ";\n" +
        "\n".join(function(layout, signature) for signature in (
            "size_t GetBlockLinearWidthGobs(",
            "template<typename Type>\n    constexpr Type CalculateBlockGobs(",
            "std::vector<MipLevelLayout> GetBlockLinearMipLayout(",
            "template<bool BlockLinearToPitch>\n    void CopyBlockLinearInternal(",
            "void CopyBlockLinearToLinear(Dimensions dimensions",
            "void CopyLinearToBlockLinear(Dimensions dimensions")).replace(
                "size_t tileWidthSpacing)", "size_t tileWidthSpacing = 0)"))
    (directory / "guest_copy.inc").write_text("namespace texture {\n" +
        "\n".join(function(layout, signature) for signature in (
            "void CopyBlockLinearToLinear(const GuestTexture &",
            "void CopyLinearToBlockLinear(const GuestTexture &",
            "void CopyPitchLinearToLinear(", "void CopyLinearToPitchLinear(")) + "\n}\n")
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
    (directory / "manager_class.inc").write_text(function((GPU / "texture_manager.h").read_text(), "class TextureManager") + ";\n")
    manager_baseline = os.environ.get("TEXMAN_BASELINE_MANAGER_REF")
    if manager_baseline:
        secondary = subprocess.run(["git", "show", manager_baseline + ":app/src/main/cpp/skyline/gpu/texture/secondary_overlap.h"],
            cwd=ROOT, text=True, capture_output=True)
        if secondary.returncode == 0:
            helper = directory / "gpu/texture/secondary_overlap.h"
            helper.parent.mkdir(parents=True, exist_ok=True)
            helper.write_text(secondary.stdout)
    manager_runtime = subprocess.check_output(["git","show",manager_baseline+":app/src/main/cpp/skyline/gpu/texture_manager.cpp"],cwd=ROOT,text=True) if manager_baseline else manager
    (directory / "manager.inc").write_text(function(manager_runtime,"TextureManager::TextureManager(")+"\n"+function(manager_runtime,"void TextureManager::SynchronizeGuestOverlaps(")+"\n"+function(manager_runtime,"std::shared_ptr<TextureView> TextureManager::FindOrCreate("))
    (directory / "depth_offset.inc").write_text(function(layout,"std::optional<size_t> GetBlockLinearDepthSliceOffset("))
    (directory / "extent.inc").write_text("return " + manager[start:end].strip() + ";")
    start = manager.index("if (matchGuestTexture.format->IsCompatible")
    end = manager.index(") {", start)
    (directory / "match.inc").write_text(manager[manager.index("const bool sameTileLayout"):start] +
                                         "return " + manager[start + 3:end + 1] + ";")
    (directory / "selection.inc").write_text(function(manager, "else if (fullMatch)"))
    (directory / "request_range.inc").write_text(function(manager, "const vk::ImageSubresourceRange requestedRange") + ";")
    (directory / "request_validation.inc").write_text(function(manager, "if (!texture::SubresourceRangeFits("))
    (directory / "unrepresentable.inc").write_text(function(manager, "if (unrepresentableMatch || separateGroup)"))
    executor = (GPU / "interconnect/command_executor.cpp").read_text()
    (directory / "alias_retry.inc").write_text(function(executor,"void CommandExecutor::HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &"))
    (directory / "attach.inc").write_text(function(executor,"bool CommandExecutor::AttachTexture("))
    (directory / "retry.inc").write_text(function(executor,
        "void CommandExecutor::HandleTextureBackingPromotion("))
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS", "-Wno-unknown-pragmas", "-Wno-unused-parameter", "-fsanitize=undefined",
                    "-fno-sanitize-recover=all", "-pthread", "-I" + str(directory),
                    "-I" + str(ROOT / "app/src/main/cpp/skyline"),
                    "-I" + os.environ.get("VULKAN_HPP_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp")),
                    "-I" + os.environ.get("VULKAN_HEADERS_INCLUDE_DIR", str(ROOT / "app/libraries/vkhpp/Vulkan-Headers/include")),
                    "-I" + os.environ.get("BOOST_INCLUDE_DIR", "/usr/include"),
                    str(ROOT / "tests/gpu_backing_promotion/tests.cpp"),
                    os.environ.get("VULKAN_LOADER_LIBRARY", "-lvulkan"),
                    "-o", str(directory / "tests")] + shlex.split(os.environ.get("CXXFLAGS", "")), check=True)
    subprocess.run([str(directory / "tests")], check=True)
