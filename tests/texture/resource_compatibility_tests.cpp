#include <array>
#include <cstdio>
#include <cstdlib>
#include <skyline/gpu/texture/resource_compatibility.h>

using namespace skyline::gpu::texture;

static void Check(TextureViewCompatibility actual, TextureViewCompatibility expected, const char *caseName) {
    if (actual != expected) {
        std::fprintf(stderr, "%s: got %d, expected %d\n", caseName, static_cast<int>(actual), static_cast<int>(expected));
        std::exit(1);
    }
}

int main() {
    const std::array parentSubresources{
        GuestSubresource{.offset = 0x1000, .size = 0x800, .width = 64, .height = 32, .depth = 1, .mip = 0, .layer = 0},
        GuestSubresource{.offset = 0x1800, .size = 0x200, .width = 32, .height = 16, .depth = 1, .mip = 1, .layer = 0},
        GuestSubresource{.offset = 0x2000, .size = 0x800, .width = 64, .height = 32, .depth = 1, .mip = 0, .layer = 1},
        GuestSubresource{.offset = 0x2800, .size = 0x200, .width = 32, .height = 16, .depth = 1, .mip = 1, .layer = 1},
    };
    const TextureResourceLayout parent{
        .tile = {.mode = TileKind::Block, .blockHeight = 2, .blockDepth = 1},
        .imageType = ImageKind::TwoDimensional,
        .viewType = ViewKind::TwoDimensionalArray,
        .layerStride = 0x1000,
        .viewMipCount = 2,
        .viewLayerCount = 2,
        .subresources = parentSubresources,
    };
    Check(ClassifyTextureViewCompatibility(parent, parent, FormatCompatibility::Exact), TextureViewCompatibility::Full, "exact image");

    const std::array mipSubresource{
        GuestSubresource{.offset = 0x1800, .size = 0x200, .width = 32, .height = 16, .depth = 1, .mip = 0, .layer = 0},
    };
    TextureResourceLayout mipView{parent};
    mipView.viewType = ViewKind::TwoDimensional;
    mipView.viewMipCount = 1;
    mipView.viewLayerCount = 1;
    mipView.subresources = mipSubresource;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::ViewCompatible, true), TextureViewCompatibility::Full, "aligned mip view");

    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::ViewCompatible), TextureViewCompatibility::LayoutIncompatible, "host format view must be verified");

    mipView.viewType = ViewKind::Cube;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::LayoutIncompatible, "cube needs host image flag and six faces");
    mipView.viewType = ViewKind::TwoDimensional;

    std::array<GuestSubresource, 12> cubeSubresources{};
    for (std::uint32_t layer{}; layer < cubeSubresources.size(); ++layer) {
        cubeSubresources[layer] = {
            .offset = 0x4000 + layer * 0x400,
            .size = 0x400,
            .width = 32,
            .height = 32,
            .depth = 1,
            .mip = 0,
            .layer = layer,
        };
    }
    const TextureResourceLayout nonCubeBacking{
        .tile = {.mode = TileKind::Pitch, .pitch = 0x80},
        .imageType = ImageKind::TwoDimensional,
        .viewType = ViewKind::TwoDimensionalArray,
        .cubeCompatible = false,
        .layerStride = 0x400,
        .viewMipCount = 1,
        .viewLayerCount = 12,
        .subresources = cubeSubresources,
    };
    auto cubeView{nonCubeBacking};
    cubeView.viewType = ViewKind::Cube;
    cubeView.viewLayerCount = 6;
    Check(ClassifyTextureViewCompatibility(nonCubeBacking, cubeView, FormatCompatibility::Exact),
          TextureViewCompatibility::CopyOnly, "valid cube view needs a separate cube-compatible backing");

    auto cubeArrayView{nonCubeBacking};
    cubeArrayView.viewType = ViewKind::CubeArray;
    Check(ClassifyTextureViewCompatibility(nonCubeBacking, cubeArrayView, FormatCompatibility::Exact),
          TextureViewCompatibility::CopyOnly, "valid cube array needs a separate cube-compatible backing");

    auto cubeCompatibleBacking{nonCubeBacking};
    cubeCompatibleBacking.cubeCompatible = true;
    Check(ClassifyTextureViewCompatibility(cubeCompatibleBacking, cubeView, FormatCompatibility::Exact),
          TextureViewCompatibility::Full, "cube-compatible backing keeps the shared-view path");
    Check(ClassifyTextureViewCompatibility(nonCubeBacking, cubeView, FormatCompatibility::ViewCompatible, true),
          TextureViewCompatibility::LayoutIncompatible, "cube CopyOnly requires an exact format");

    auto invalidCubeView{cubeView};
    invalidCubeView.viewLayerCount = 5;
    Check(ClassifyTextureViewCompatibility(nonCubeBacking, invalidCubeView, FormatCompatibility::Exact),
          TextureViewCompatibility::LayoutIncompatible, "cube CopyOnly requires exactly six faces");
    invalidCubeView = cubeView;
    invalidCubeView.viewLayerBase = 1;
    Check(ClassifyTextureViewCompatibility(nonCubeBacking, invalidCubeView, FormatCompatibility::Exact),
          TextureViewCompatibility::LayoutIncompatible, "cube CopyOnly requires an aligned face base");

    auto partialCubeSubresources{cubeSubresources};
    partialCubeSubresources[3].offset += 0x100;
    partialCubeSubresources[3].size -= 0x100;
    invalidCubeView = cubeView;
    invalidCubeView.subresources = partialCubeSubresources;
    Check(ClassifyTextureViewCompatibility(nonCubeBacking, invalidCubeView, FormatCompatibility::Exact),
          TextureViewCompatibility::LayoutIncompatible, "partial cube overlap is not CopyOnly");

    auto depthCubeSubresources{cubeSubresources};
    depthCubeSubresources[0].depth = 2;
    invalidCubeView = cubeView;
    invalidCubeView.subresources = depthCubeSubresources;
    Check(ClassifyTextureViewCompatibility(nonCubeBacking, invalidCubeView, FormatCompatibility::Exact),
          TextureViewCompatibility::LayoutIncompatible, "cube CopyOnly excludes depth slices");

    const std::array shiftedMip{
        GuestSubresource{.offset = 0x1810, .size = 0x200, .width = 32, .height = 16, .depth = 1, .mip = 0, .layer = 0},
    };
    mipView.subresources = shiftedMip;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::LayoutIncompatible, "unaligned mip alias");
    mipView.subresources = mipSubresource;

    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::CopyCompatible), TextureViewCompatibility::CopyOnly, "copyable format requires separate storage");
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Incompatible), TextureViewCompatibility::Incompatible, "incompatible format");

    const std::array oneDimensionalSubresource{
        GuestSubresource{.offset = 0x3000, .size = 0x100, .width = 64, .height = 1, .depth = 1, .mip = 0, .layer = 0},
    };
    const TextureResourceLayout oneDimensional{
        .tile = {.mode = TileKind::Pitch, .pitch = 0x100},
        .imageType = ImageKind::OneDimensional,
        .viewType = ViewKind::OneDimensional,
        .layerStride = 0x100,
        .viewMipCount = 1,
        .viewLayerCount = 1,
        .subresources = oneDimensionalSubresource,
    };
    auto heightOneTwoDimensional{oneDimensional};
    heightOneTwoDimensional.imageType = ImageKind::TwoDimensional;
    heightOneTwoDimensional.viewType = ViewKind::TwoDimensional;
    Check(ClassifyTextureViewCompatibility(oneDimensional, heightOneTwoDimensional, FormatCompatibility::Exact),
          TextureViewCompatibility::CopyOnly, "1D and height-one 2D require separate copy-related images");
    Check(ClassifyTextureViewCompatibility(heightOneTwoDimensional, oneDimensional, FormatCompatibility::Exact),
          TextureViewCompatibility::CopyOnly, "1D/2D semantic equivalence is independent of copy direction");
    Check(ClassifyTextureViewCompatibility(oneDimensional, heightOneTwoDimensional, FormatCompatibility::Incompatible),
          TextureViewCompatibility::Incompatible, "image dimensionality does not override incompatible formats");
    Check(ClassifyTextureViewCompatibility(oneDimensional, oneDimensional, FormatCompatibility::Exact),
          TextureViewCompatibility::Full, "same 1D image remains full");

    const std::array partialOneDimensional{
        GuestSubresource{.offset = 0x3080, .size = 0x80, .width = 32, .height = 1, .depth = 1, .mip = 0, .layer = 0},
    };
    heightOneTwoDimensional.subresources = partialOneDimensional;
    Check(ClassifyTextureViewCompatibility(oneDimensional, heightOneTwoDimensional, FormatCompatibility::Exact),
          TextureViewCompatibility::LayoutIncompatible, "partial 1D/2D overlap is not copy-equivalent");
    heightOneTwoDimensional.subresources = oneDimensionalSubresource;

    const std::array heightTwoSubresource{
        GuestSubresource{.offset = 0x3000, .size = 0x100, .width = 64, .height = 2, .depth = 1, .mip = 0, .layer = 0},
    };
    heightOneTwoDimensional.subresources = heightTwoSubresource;
    Check(ClassifyTextureViewCompatibility(oneDimensional, heightOneTwoDimensional, FormatCompatibility::Exact),
          TextureViewCompatibility::LayoutIncompatible, "non-unit 2D height is not 1D copy-equivalent");
    heightOneTwoDimensional.subresources = oneDimensionalSubresource;

    auto threeDimensionalSlice{heightOneTwoDimensional};
    threeDimensionalSlice.imageType = ImageKind::ThreeDimensional;
    threeDimensionalSlice.viewType = ViewKind::ThreeDimensional;
    Check(ClassifyTextureViewCompatibility(heightOneTwoDimensional, threeDimensionalSlice, FormatCompatibility::Exact),
          TextureViewCompatibility::LayoutIncompatible, "3D slice handling remains outside CopyOnly classification");

    mipView.tile.blockHeight = 4;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::LayoutIncompatible, "different block geometry");
    mipView.tile.blockHeight = 2;

    mipView.imageType = ImageKind::ThreeDimensional;
    mipView.viewType = ViewKind::ThreeDimensional;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::LayoutIncompatible, "different image types");
    mipView.imageType = ImageKind::TwoDimensional;
    mipView.viewType = ViewKind::TwoDimensional;

    const std::array disjoint{
        GuestSubresource{.offset = 0x4000, .size = 0x200, .width = 32, .height = 16, .depth = 1, .mip = 0, .layer = 0},
    };
    mipView.subresources = disjoint;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::Incompatible, "no guest overlap");

    const std::array twoLayers{parentSubresources[0], parentSubresources[2]};
    mipView.subresources = twoLayers;
    mipView.viewType = ViewKind::TwoDimensionalArray;
    mipView.viewLayerCount = 2;
    mipView.layerStride = 0x800;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::LayoutIncompatible, "inconsistent layer stride");

    const std::array duplicateMip{parentSubresources[0], parentSubresources[0]};
    mipView.subresources = duplicateMip;
    mipView.viewMipCount = 2;
    mipView.viewLayerCount = 1;
    mipView.layerStride = parent.layerStride;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::LayoutIncompatible, "incomplete mip set");

    std::puts("texture compatibility tests passed");
}
