// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <optional>
#include <vector>
#include "resource_compatibility.h"

namespace skyline::gpu::texture {
    struct MipDescription {
        std::uint32_t width{}, height{}, depth{};
        std::size_t guestSize{};
        std::uint32_t blockHeight{}, blockDepth{};
    };

    struct OwnedTextureResourceLayout {
        TextureResourceLayout info{};
        std::vector<GuestSubresource> subresources{};

        TextureResourceLayout Layout() const {
            auto layout{info};
            layout.subresources = subresources;
            return layout;
        }
    };

    /** Build real mip/layer physical spans using logical offsets into guest mappings. */
    inline std::optional<OwnedTextureResourceLayout> BuildResourceLayout(
        const GuestResourceRanges &ranges, std::span<const MipDescription> mips,
        std::uint32_t layerCount, TextureResourceLayout info) {
        if (!ranges.Valid() || mips.empty() || !layerCount || !info.layerStride ||
            info.viewMipBase >= mips.size() || !info.viewMipCount ||
            info.viewMipCount > mips.size() - info.viewMipBase ||
            info.viewLayerBase >= layerCount || !info.viewLayerCount ||
            info.viewLayerCount > layerCount - info.viewLayerBase ||
            !ValidViewType(info.imageType, info.viewType) ||
            info.layerStride > std::numeric_limits<std::size_t>::max() / layerCount)
            return std::nullopt;

        if (info.tile.mode == TileKind::Block &&
            (!info.tile.blockHeight || !info.tile.blockDepth ||
             mips.front().blockHeight != info.tile.blockHeight ||
             mips.front().blockDepth != info.tile.blockDepth))
            return std::nullopt;

        OwnedTextureResourceLayout result{info};
        result.subresources.reserve(mips.size() * layerCount);
        for (std::uint32_t layer{}; layer < layerCount; ++layer) {
            const auto base = std::size_t{layer} * info.layerStride;
            std::size_t levelOffset{};
            for (std::size_t mip{}; mip < mips.size(); ++mip) {
                const auto &level = mips[mip];
                if (!level.width || !level.height || !level.depth || !level.guestSize ||
                    (info.tile.mode == TileKind::Block && (!level.blockHeight || !level.blockDepth)) ||
                    level.guestSize > info.layerStride - levelOffset ||
                    base > ranges.Size() || levelOffset > ranges.Size() - base ||
                    level.guestSize > ranges.Size() - base - levelOffset)
                    return std::nullopt;
                const auto resourceOffset{base + levelOffset};
                auto segments = ranges.Slice(resourceOffset, level.guestSize);
                if (segments.empty())
                    return std::nullopt;
                // offset is diagnostic for split spans. Classification always uses segments.
                GuestSubresource subresource{.offset = segments.front().address,
                    .size = level.guestSize, .width = level.width, .height = level.height,
                    .depth = level.depth, .mip = static_cast<std::uint32_t>(mip), .layer = layer,
                    .segments = std::move(segments),
                    .blockHeight = level.blockHeight, .blockDepth = level.blockDepth};

                if (info.tile.mode == TileKind::Block &&
                    info.formatBlockWidth && info.formatBlockHeight && info.formatBytesPerBlock) {
                    const auto divideCeil = [](std::size_t value, std::size_t divisor) {
                        return value / divisor + static_cast<std::size_t>(value % divisor != 0);
                    };
                    const auto widthBlocks{divideCeil(level.width, info.formatBlockWidth)};
                    if (widthBlocks > std::numeric_limits<std::size_t>::max() /
                            info.formatBytesPerBlock)
                        return std::nullopt;
                    const auto widthBytes{widthBlocks * info.formatBytesPerBlock};
                    const auto widthGobs{divideCeil(widthBytes, std::size_t{64})};
                    const auto heightBlocks{divideCeil(level.height, info.formatBlockHeight)};
                    const auto heightGobs{divideCeil(heightBlocks, std::size_t{8})};
                    const auto blocksInY{divideCeil(heightGobs, level.blockHeight)};
                    if (!widthGobs || !blocksInY ||
                        widthGobs > std::numeric_limits<std::size_t>::max() / blocksInY ||
                        level.blockHeight > std::numeric_limits<std::size_t>::max() / 512)
                        return std::nullopt;
                    const auto blockCount{widthGobs * blocksInY};
                    const auto gobColumnSize{std::size_t{512} * level.blockHeight};
                    if (blockCount > std::numeric_limits<std::size_t>::max() / gobColumnSize ||
                        level.blockDepth > std::numeric_limits<std::size_t>::max() / gobColumnSize)
                        return std::nullopt;
                    const auto slicePlaneSize{blockCount * gobColumnSize};
                    const auto blockStride{std::size_t{level.blockDepth} * gobColumnSize};

                    subresource.depthSlices.resize(level.depth);
                    for (std::size_t slice{}; slice < level.depth; ++slice) {
                        const auto sliceInBlock{slice % level.blockDepth};
                        const auto blockBaseSlice{slice - sliceInBlock};
                        if (blockBaseSlice > std::numeric_limits<std::size_t>::max() / slicePlaneSize)
                            return std::nullopt;
                        const auto blockBase{blockBaseSlice * slicePlaneSize};
                        if (sliceInBlock > (std::numeric_limits<std::size_t>::max() - blockBase) /
                                gobColumnSize)
                            return std::nullopt;
                        const auto first{blockBase + sliceInBlock * gobColumnSize};
                        auto &depthSlice{subresource.depthSlices[slice]};
                        bool complete{true};
                        for (std::size_t block{}; block < blockCount; ++block) {
                            if (block > (std::numeric_limits<std::size_t>::max() - first) /
                                    blockStride) {
                                complete = false;
                                break;
                            }
                            const auto relative{first + block * blockStride};
                            if (relative > level.guestSize ||
                                gobColumnSize > level.guestSize - relative) {
                                complete = false;
                                break;
                            }
                            auto pieces{ranges.Slice(resourceOffset + relative, gobColumnSize)};
                            if (pieces.empty()) {
                                complete = false;
                                break;
                            }
                            depthSlice.segments.insert(depthSlice.segments.end(),
                                pieces.begin(), pieces.end());
                        }
                        if (complete)
                            depthSlice.size = blockCount * gobColumnSize;
                        else
                            depthSlice = {};
                    }
                }

                result.subresources.push_back(std::move(subresource));
                levelOffset += level.guestSize;
            }
        }
        return result;
    }

    struct ResolvedViewBase { std::uint32_t mip{}, layer{}; };

    struct ResolvedSubresource {
        std::uint32_t mip{}, layer{}, depthSlice{};

        bool operator==(const ResolvedSubresource &) const = default;
    };

    struct ResolvedCopySubresource {
        ResolvedSubresource backing{};
        ResolvedSubresource requested{};

        bool operator==(const ResolvedCopySubresource &) const = default;
    };

    /** A proven subresource-to-subresource relationship between separate host images. */
    struct ResolvedCopyRegion {
        std::vector<ResolvedCopySubresource> subresources{};
    };

    inline bool ContainsSubresource(const TextureResourceLayout &layout,
                                    ResolvedSubresource resolved) {
        for (const auto &subresource : layout.subresources)
            if (subresource.mip == resolved.mip && subresource.layer == resolved.layer)
                return resolved.depthSlice < subresource.depth;
        return false;
    }

    inline const GuestSubresource *FindSubresource(
        const TextureResourceLayout &layout, ResolvedSubresource resolved) {
        const GuestSubresource *result{};
        for (const auto &subresource : layout.subresources) {
            if (subresource.mip != resolved.mip || subresource.layer != resolved.layer)
                continue;
            if (result || resolved.depthSlice >= subresource.depth)
                return nullptr;
            result = &subresource;
        }
        return result;
    }

    inline bool IsExactBlockLinearDepthSliceRelation(
        const TextureResourceLayout &firstLayout, ResolvedSubresource first,
        const TextureResourceLayout &secondLayout, ResolvedSubresource second) {
        const bool first3D{firstLayout.imageType == ImageKind::ThreeDimensional};
        const bool second3D{secondLayout.imageType == ImageKind::ThreeDimensional};
        if (first3D == second3D ||
            (firstLayout.imageType != ImageKind::TwoDimensional && !first3D) ||
            (secondLayout.imageType != ImageKind::TwoDimensional && !second3D) ||
            firstLayout.tile.mode != TileKind::Block || secondLayout.tile.mode != TileKind::Block ||
            (first3D ? first.layer != 0 || second.depthSlice != 0
                     : second.layer != 0 || first.depthSlice != 0))
            return false;

        const auto firstDescription{FindSubresource(firstLayout, first)};
        const auto secondDescription{FindSubresource(secondLayout, second)};
        if (!firstDescription || !secondDescription ||
            firstDescription->width != secondDescription->width ||
            firstDescription->height != secondDescription->height ||
            firstDescription->blockHeight != secondDescription->blockHeight ||
            firstDescription->blockDepth != secondDescription->blockDepth ||
            firstDescription->depthSlices.size() != firstDescription->depth ||
            secondDescription->depthSlices.size() != secondDescription->depth)
            return false;

        const auto &firstSlice{firstDescription->depthSlices[first.depthSlice]};
        const auto &secondSlice{secondDescription->depthSlices[second.depthSlice]};
        return SameGuestBytes(firstSlice.size, firstSlice.segments,
                              secondSlice.size, secondSlice.segments);
    }

    /** Resolve the same backing mip/layer for every selected guest subresource. */
    inline std::optional<ResolvedViewBase> ResolveFullView(const TextureResourceLayout &backing,
                                                            const TextureResourceLayout &requested) {
        std::optional<ResolvedViewBase> base;
        std::size_t matched{};
        for (const auto &view : requested.subresources) {
            if (view.mip < requested.viewMipBase || view.mip - requested.viewMipBase >= requested.viewMipCount ||
                view.layer < requested.viewLayerBase || view.layer - requested.viewLayerBase >= requested.viewLayerCount)
                continue;
            const GuestSubresource *found{};
            for (const auto &candidate : backing.subresources)
                if (SameGuestBytes(view, candidate) && view.width == candidate.width &&
                    view.height == candidate.height && view.depth == candidate.depth) {
                    if (found) return std::nullopt;
                    found = &candidate;
                }
            if (!found || found->mip < view.mip - requested.viewMipBase ||
                found->layer < view.layer - requested.viewLayerBase)
                return std::nullopt;
            ResolvedViewBase current{found->mip - (view.mip - requested.viewMipBase),
                                     found->layer - (view.layer - requested.viewLayerBase)};
            if (base && (base->mip != current.mip || base->layer != current.layer))
                return std::nullopt;
            base = current;
            ++matched;
        }
        if (matched != std::size_t{requested.viewMipCount} * requested.viewLayerCount || !base)
            return std::nullopt;
        return base;
    }

    /** Resolve every selected requested mip/layer to one exact backing subresource. */
    inline std::optional<ResolvedCopyRegion> ResolveCopyRegion(const TextureResourceLayout &backing,
                                                                const TextureResourceLayout &requested) {
        if (const auto depthSlice = FindExactBlockLinearDepthSlice(backing, requested)) {
            return ResolvedCopyRegion{.subresources = {{
                .backing = {
                    .mip = depthSlice->backingMip,
                    .layer = depthSlice->backingLayer,
                    .depthSlice = depthSlice->backingDepthSlice,
                },
                .requested = {
                    .mip = depthSlice->requestedMip,
                    .layer = depthSlice->requestedLayer,
                    .depthSlice = depthSlice->requestedDepthSlice,
                },
            }}};
        }

        ResolvedCopyRegion region;
        region.subresources.reserve(std::size_t{requested.viewMipCount} * requested.viewLayerCount);
        for (const auto &view : requested.subresources) {
            if (view.mip < requested.viewMipBase || view.mip - requested.viewMipBase >= requested.viewMipCount ||
                view.layer < requested.viewLayerBase || view.layer - requested.viewLayerBase >= requested.viewLayerCount)
                continue;
            const GuestSubresource *found{};
            for (const auto &candidate : backing.subresources)
                if (SameGuestBytes(view, candidate) && view.width == candidate.width &&
                    view.height == candidate.height && view.depth == candidate.depth) {
                    if (found)
                        return std::nullopt;
                    found = &candidate;
                }
            if (!found)
                return std::nullopt;
            region.subresources.push_back({
                .backing = {.mip = found->mip, .layer = found->layer, .depthSlice = 0},
                .requested = {.mip = view.mip, .layer = view.layer, .depthSlice = 0},
            });
        }
        if (region.subresources.size() != std::size_t{requested.viewMipCount} * requested.viewLayerCount)
            return std::nullopt;

        return region;
    }

    struct ClassifiedResourceView {
        TextureViewCompatibility relation{};
        std::optional<ResolvedViewBase> sharedView{};
        std::optional<ResolvedCopyRegion> copyRegion{};
    };

    inline ClassifiedResourceView ClassifyAndResolveView(const TextureResourceLayout &backing,
                                                          const TextureResourceLayout &requested,
                                                          FormatCompatibility format, bool supportsFormatView) {
        auto relation = ClassifyTextureViewCompatibility(backing, requested, format, supportsFormatView);
        if (relation != TextureViewCompatibility::Full && relation != TextureViewCompatibility::CopyOnly)
            return {relation, std::nullopt, std::nullopt};
        if (relation == TextureViewCompatibility::Full) {
            auto resolved = ResolveFullView(backing, requested);
            if (!resolved)
                return {TextureViewCompatibility::LayoutIncompatible, std::nullopt, std::nullopt};
            return {relation, resolved, std::nullopt};
        }
        if (relation == TextureViewCompatibility::CopyOnly) {
            auto resolved = ResolveCopyRegion(backing, requested);
            if (!resolved)
                return {TextureViewCompatibility::LayoutIncompatible, std::nullopt, std::nullopt};
            return {relation, std::nullopt, std::move(resolved)};
        }
        return {relation, std::nullopt, std::nullopt};
    }

    /**
     * Confirm that a Full classification resolves to the subresource selected by the active
     * lookup policy. Until alias validity is tracked, another compatible representation cannot
     * safely replace that selection because its host contents may be older.
     */
    inline std::optional<ResolvedViewBase> ConfirmFullViewAgainstLegacy(
        const ClassifiedResourceView &classified, ResolvedViewBase legacySelection) {
        if (classified.relation != TextureViewCompatibility::Full || !classified.sharedView ||
            classified.sharedView->mip != legacySelection.mip ||
            classified.sharedView->layer != legacySelection.layer)
            return std::nullopt;
        return classified.sharedView;
    }
}
