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
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::ViewCompatible), TextureViewCompatibility::Full, "aligned mip view");

    const std::array shiftedMip{
        GuestSubresource{.offset = 0x1810, .size = 0x200, .width = 32, .height = 16, .depth = 1, .mip = 0, .layer = 0},
    };
    mipView.subresources = shiftedMip;
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Exact), TextureViewCompatibility::LayoutIncompatible, "unaligned mip alias");
    mipView.subresources = mipSubresource;

    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::CopyCompatible), TextureViewCompatibility::CopyOnly, "copyable format requires separate storage");
    Check(ClassifyTextureViewCompatibility(parent, mipView, FormatCompatibility::Incompatible), TextureViewCompatibility::Incompatible, "incompatible format");

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
