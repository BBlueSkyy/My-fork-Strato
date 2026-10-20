#!/usr/bin/env python3
"""Exercise exact TIC construction, GuestTexture sizing and swizzle production code."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
GPU = ROOT / "app/src/main/cpp/skyline/gpu"


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix="strato-sampled-layout-") as directory:
    build = pathlib.Path(directory)
    texture = (GPU / "texture/texture.cpp").read_text()
    layout = (GPU / "texture/layout.cpp").read_text()
    textures = (GPU / "interconnect/common/textures.cpp").read_text()
    metadata = (GPU / "texture/texture.h").read_text()
    (build / "tile.inc").write_text(function(metadata, "enum class TileMode") + ";\n" +
                                  function(metadata, "struct TileConfig") + ";\n")
    (build / "guest.inc").write_text("namespace skyline::gpu {\n" + "\n".join(
        function(texture, signature) for signature in (
            "u32 GuestTexture::GetLayerStride(", "u32 GuestTexture::CalculateLayerSize(",
            "size_t GuestTexture::GetSize(", "vk::ImageType GuestTexture::GetImageType(",
            "u32 GuestTexture::GetViewLayerCount(", "u32 GuestTexture::GetViewDepth(")) + "\n}\n")
    constants = layout[layout.index("    constexpr size_t SectorWidth"):layout.index("    size_t GetBlockLinearLayerSize")]
    (build / "layout.inc").write_text("namespace skyline::gpu::texture {\n" + constants + function(metadata, "struct MipLevelLayout") + ";\n" + "\n".join(
        function(layout, signature) for signature in (
            "template<typename Type>",
            "size_t GetBlockLinearLayerSize(Dimensions dimensions, size_t formatBlockHeight",
            "std::vector<MipLevelLayout> GetBlockLinearMipLayout(",
            "template<bool BlockLinearToPitch>",
            "void CopyBlockLinearToLinear(Dimensions dimensions",
            "void CopyBlockLinearToLinear(const GuestTexture &guest")) + "\n}\n")
    start = textures.index("            constexpr size_t CubeFaceCount", textures.index("TextureView *Textures::GetTexture("))
    # Exclude GPU mapping/size validation: GetSize() materializes the lazy
    # layerStride, which is intentionally unset after constructing a mipmapped TIC.
    end = textures.index("            // End TIC construction:", start)
    # This is the unchanged complete production dimensions/type/tiling setup.
    (build / "tic-construction.inc").write_text(textures[start:end])
    manager = (GPU / "texture_manager.cpp").read_text()
    start = manager.index("if (matchGuestTexture.format->IsCompatible")
    # Include the complete if/else that selects full-match, alias or legacy readback.
    first_if = function(manager[start:], "if (matchGuestTexture.format->IsCompatible")
    following = manager[start + len(first_if):]
    selection = first_if + " " + function(following, "else")
    (build / "alias-selection.inc").write_text(selection)
    (build / "alias-registration.inc").write_text(function(manager, "if (unrepresentableMatch || separateGroup)"))
    (build / "alias-methods.inc").write_text("\n".join(function(texture, signature) for signature in (
        "bool Texture::CanMaterializeGuestLayout(", "bool Texture::IsGuestLayoutReversible(",
        "bool Texture::CanReconcileGuestLayout(", "std::shared_ptr<texture::AliasGroup> Texture::RegisterAliases(",
        "void Texture::RequireAliasCoherence(", "void Texture::ReconcileAliases(",
        "void Texture::RecordGpuWrite(", "std::shared_ptr<void> Texture::PinGuestMemory(")))
    executor = (GPU / "interconnect/command_executor.cpp").read_text()
    (build / "alias-retry.inc").write_text(function(executor,
        "void CommandExecutor::HandleTextureBackingPromotion(const TextureAliasReconciliationRequest &"))
    (build / "common").mkdir()
    (build / "common/base.h").write_text("#pragma once\n")
    flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-return-type", "-Wno-implicit-fallthrough", "-Wno-unused-variable", "-Wno-unknown-pragmas"]
    if os.environ.get("SANITIZE", "1") != "0":
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run([os.environ.get("CXX", "g++"), *flags, "-I" + str(build),
                    "-I" + str(ROOT / "app/src/main/cpp/skyline"),
                    str(ROOT / "tests/gpu_sampled_layout/tests.cpp"), "-o", str(build / "tests")], check=True)
    subprocess.run([str(build / "tests")], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    subprocess.run([os.environ.get("CXX", "g++"), *flags, "-pthread", "-I" + str(build),
                    "-I" + str(ROOT / "app/src/main/cpp/skyline"),
                    str(ROOT / "tests/gpu_sampled_layout/alias_tests.cpp"), "-o", str(build / "alias-tests")], check=True)
    subprocess.run([str(build / "alias-tests")], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
