#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <memory>
#include <skyline/gpu/texture/resource_layout.h>
#include <skyline/gpu/texture/mapping_cache.h>

namespace skyline::gpu::texture { class TextureStorage {}; }

using namespace skyline::gpu::texture;

int main() {
    std::array<std::uint8_t, 8192> memory{};
    const std::array<std::span<std::uint8_t>, 2> parentMappings{
        std::span{memory}.subspan(128, 80), std::span{memory}.subspan(2048, 304),
    };
    GuestResourceRanges parentRanges{parentMappings};
    const std::array<MipDescription, 2> levels{{{64, 32, 1, 64, 2, 1}, {32, 16, 1, 32, 1, 1}}};
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
    mipInfo.tile.blockHeight = 1; // The parent mip reduced its GOB block height.
    const std::array<MipDescription, 1> small{{{32, 16, 1, 32, 1, 1}}};
    auto mip = BuildResourceLayout(GuestResourceRanges{mipMappings}, small, 1, mipInfo);
    assert(mip);
    assert(ClassifyTextureViewCompatibility(full, mip->Layout(), FormatCompatibility::Exact) == TextureViewCompatibility::Full);
    auto mipBase = ResolveFullView(full, mip->Layout());
    assert(mipBase && mipBase->mip == 1 && mipBase->layer == 0);
    assert(ClassifyTextureViewCompatibility(full, mip->Layout(), FormatCompatibility::CopyCompatible) == TextureViewCompatibility::CopyOnly);

    // Identical bytes and dimensions are insufficient when the effective GOB layout differs.
    auto wrongHeightInfo = mipInfo;
    wrongHeightInfo.tile.blockHeight = 2;
    const std::array<MipDescription, 1> wrongHeight{{{32, 16, 1, 32, 2, 1}}};
    auto wrongHeightView = BuildResourceLayout(GuestResourceRanges{mipMappings}, wrongHeight, 1, wrongHeightInfo);
    assert(wrongHeightView);
    const auto heightResult = ClassifyAndResolveView(full, wrongHeightView->Layout(), FormatCompatibility::Exact, true);
    assert(heightResult.relation == TextureViewCompatibility::LayoutIncompatible && !heightResult.sharedView);

    auto wrongDepthInfo = mipInfo;
    wrongDepthInfo.tile.blockDepth = 2;
    const std::array<MipDescription, 1> wrongDepth{{{32, 16, 1, 32, 1, 2}}};
    auto wrongDepthView = BuildResourceLayout(GuestResourceRanges{mipMappings}, wrongDepth, 1, wrongDepthInfo);
    assert(wrongDepthView);
    const auto depthResult = ClassifyAndResolveView(full, wrongDepthView->Layout(), FormatCompatibility::Exact, true);
    assert(depthResult.relation == TextureViewCompatibility::LayoutIncompatible && !depthResult.sharedView);

    // A classified incompatible candidate remains present in the legacy mapping lookup.
    TextureMappingCache mappingCache;
    auto storage = std::make_shared<TextureStorage>();
    mappingCache.Insert(storage, parentRanges);
    auto candidateLookup = mappingCache.Lookup(GuestResourceRanges{mipMappings});
    assert(candidateLookup.storages.size() == 1 && candidateLookup.storages.front() == storage);
    assert(!candidateLookup.firstMappingOverlaps.empty() && candidateLookup.firstMappingOverlaps.front()->storage == storage);
    assert(!heightResult.sharedView);

    // A matching selected mip is insufficient if the rest of the resource has a foreign tail.
    const std::array<std::span<std::uint8_t>, 2> foreignTail{
        std::span{memory}.subspan(128, 64), std::span{memory}.subspan(4096, 32),
    };
    auto shortInfo = parentInfo;
    shortInfo.viewMipCount = 1;
    shortInfo.viewLayerCount = 1;
    shortInfo.layerStride = 96;
    auto partialResource = BuildResourceLayout(GuestResourceRanges{foreignTail}, levels, 1, shortInfo);
    assert(partialResource);
    assert(ClassifyAndResolveView(full, partialResource->Layout(), FormatCompatibility::Exact, true).sharedView);
    assert(!parentRanges.FindContainedOffset(GuestResourceRanges{foreignTail}));

    const std::array<std::span<std::uint8_t>, 1> layerMappings{std::span{memory}.subspan(2160, 96)};
    TextureResourceLayout layerInfo{parentInfo};
    layerInfo.viewMipCount = 2;
    layerInfo.viewLayerCount = 1;
    layerInfo.viewType = ViewKind::TwoDimensional;
    auto layer = BuildResourceLayout(GuestResourceRanges{layerMappings}, levels, 1, layerInfo);
    assert(layer);
    auto layerBase = ResolveFullView(full, layer->Layout());
    assert(layerBase && layerBase->mip == 0 && layerBase->layer == 1);

    const std::array<std::span<std::uint8_t>, 1> layerMipMapping{std::span{memory}.subspan(2224, 32)};
    auto layerMip = BuildResourceLayout(GuestResourceRanges{layerMipMapping}, small, 1, mipInfo);
    assert(layerMip);
    const auto layerMipResult = ClassifyAndResolveView(full, layerMip->Layout(), FormatCompatibility::Exact, true);
    assert(layerMipResult.relation == TextureViewCompatibility::Full && layerMipResult.sharedView &&
           layerMipResult.sharedView->mip == 1 && layerMipResult.sharedView->layer == 1);
    assert(parentRanges.FindContainedOffset(GuestResourceRanges{layerMipMapping}) == 256);

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
