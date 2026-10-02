#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <skyline/gpu/texture/resource_layout.h>

using namespace skyline::gpu::texture;

int main() {
    std::array<std::uint8_t, 8192> memory{};
    const std::array<std::span<std::uint8_t>, 2> parentMappings{
        std::span{memory}.subspan(128, 80), std::span{memory}.subspan(2048, 304),
    };
    GuestResourceRanges parentRanges{parentMappings};
    const std::array<MipDescription, 2> levels{{{64, 32, 1, 64}, {32, 16, 1, 32}}};
    const TextureResourceLayout parentInfo{
        .tile = {.mode = TileKind::Block, .blockHeight = 2, .blockDepth = 1},
        .imageType = ImageKind::TwoDimensional, .viewType = ViewKind::TwoDimensionalArray,
        .layerStride = 192, .viewMipCount = 2, .viewLayerCount = 2,
    };
    auto parent = BuildResourceLayout(parentRanges, levels, 2, parentInfo);
    assert(parent);
    const auto full = parent->Layout();
    assert(full.subresources.size() == 4);
    assert(full.subresources[1].segments.size() == 2); // Mip 1 crosses a mapping boundary.
    assert(full.subresources[1].segments[0].size == 16);
    assert(full.subresources[1].segments[1].address == reinterpret_cast<std::uintptr_t>(memory.data() + 2048));
    assert(ClassifyTextureViewCompatibility(full, full, FormatCompatibility::Exact) == TextureViewCompatibility::Full);
    auto exactBase = ResolveFullView(full, full);
    assert(exactBase && exactBase->mip == 0 && exactBase->layer == 0);

    const std::array<std::span<std::uint8_t>, 2> mipMappings{
        std::span{memory}.subspan(192, 16), std::span{memory}.subspan(2048, 16),
    };
    TextureResourceLayout mipInfo{parentInfo};
    mipInfo.viewType = ViewKind::TwoDimensional;
    mipInfo.viewMipCount = 1;
    mipInfo.viewLayerCount = 1;
    mipInfo.layerStride = 32;
    const std::array<MipDescription, 1> small{{{32, 16, 1, 32}}};
    auto mip = BuildResourceLayout(GuestResourceRanges{mipMappings}, small, 1, mipInfo);
    assert(mip);
    assert(ClassifyTextureViewCompatibility(full, mip->Layout(), FormatCompatibility::Exact) == TextureViewCompatibility::Full);
    auto mipBase = ResolveFullView(full, mip->Layout());
    assert(mipBase && mipBase->mip == 1 && mipBase->layer == 0);
    assert(ClassifyTextureViewCompatibility(full, mip->Layout(), FormatCompatibility::CopyCompatible) == TextureViewCompatibility::CopyOnly);

    const std::array<std::span<std::uint8_t>, 1> layerMappings{std::span{memory}.subspan(2160, 96)};
    TextureResourceLayout layerInfo{parentInfo};
    layerInfo.viewMipCount = 2;
    layerInfo.viewLayerCount = 1;
    layerInfo.viewType = ViewKind::TwoDimensional;
    auto layer = BuildResourceLayout(GuestResourceRanges{layerMappings}, levels, 1, layerInfo);
    assert(layer);
    auto layerBase = ResolveFullView(full, layer->Layout());
    assert(layerBase && layerBase->mip == 0 && layerBase->layer == 1);

    const std::array<std::span<std::uint8_t>, 2> largerMappings{
        std::span{memory}.subspan(128, 80), std::span{memory}.subspan(2048, 496),
    };
    auto larger = BuildResourceLayout(GuestResourceRanges{largerMappings}, levels, 3, parentInfo);
    assert(larger);
    const std::array<std::span<std::uint8_t>, 1> arrayMappings{std::span{memory}.subspan(2160, 384)};
    auto arrayView = BuildResourceLayout(GuestResourceRanges{arrayMappings}, levels, 2, parentInfo);
    assert(arrayView);
    assert(ClassifyTextureViewCompatibility(larger->Layout(), arrayView->Layout(), FormatCompatibility::Exact) == TextureViewCompatibility::Full);
    auto arrayBase = ResolveFullView(larger->Layout(), arrayView->Layout());
    assert(arrayBase && arrayBase->mip == 0 && arrayBase->layer == 1);

    const std::array<std::span<std::uint8_t>, 1> partialMappings{std::span{memory}.subspan(2152, 96)};
    auto partial = BuildResourceLayout(GuestResourceRanges{partialMappings}, levels, 1, layerInfo);
    assert(partial);
    assert(ClassifyTextureViewCompatibility(full, partial->Layout(), FormatCompatibility::Exact) == TextureViewCompatibility::LayoutIncompatible);
    assert(!ResolveFullView(full, partial->Layout()));

    auto tileMismatch = mip->Layout();
    tileMismatch.tile.blockHeight = 4;
    assert(ClassifyTextureViewCompatibility(full, tileMismatch, FormatCompatibility::Exact) == TextureViewCompatibility::LayoutIncompatible);
    auto strideMismatch = parent->Layout();
    strideMismatch.layerStride += 64;
    assert(ClassifyTextureViewCompatibility(full, strideMismatch, FormatCompatibility::Exact) == TextureViewCompatibility::LayoutIncompatible);
    assert(ClassifyTextureViewCompatibility(full, mip->Layout(), FormatCompatibility::ViewCompatible, true) == TextureViewCompatibility::Full);
    assert(ClassifyTextureViewCompatibility(full, mip->Layout(), FormatCompatibility::ViewCompatible, false) == TextureViewCompatibility::LayoutIncompatible);
    const auto selected = ClassifyAndResolveView(full, mip->Layout(), FormatCompatibility::Exact, true);
    assert(selected.relation == TextureViewCompatibility::Full && selected.sharedView && selected.sharedView->mip == 1);
    const auto copy = ClassifyAndResolveView(full, mip->Layout(), FormatCompatibility::CopyCompatible, true);
    assert(copy.relation == TextureViewCompatibility::CopyOnly && !copy.sharedView);
    const auto unsupported = ClassifyAndResolveView(full, mip->Layout(), FormatCompatibility::ViewCompatible, false);
    assert(unsupported.relation == TextureViewCompatibility::LayoutIncompatible && !unsupported.sharedView);
    const auto partialRelation = ClassifyAndResolveView(full, partial->Layout(), FormatCompatibility::Exact, true);
    assert(partialRelation.relation == TextureViewCompatibility::LayoutIncompatible && !partialRelation.sharedView);

    // A sliced view cannot claim a full mip unless all its physical fragments are present.
    const std::array<std::span<std::uint8_t>, 1> missingTail{std::span{memory}.subspan(192, 16)};
    assert(!BuildResourceLayout(GuestResourceRanges{missingTail}, small, 1, mipInfo));
}
