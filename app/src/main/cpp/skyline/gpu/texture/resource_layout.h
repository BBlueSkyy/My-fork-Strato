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
                auto segments = ranges.Slice(base + levelOffset, level.guestSize);
                if (segments.empty())
                    return std::nullopt;
                // offset is diagnostic for split spans. Classification always uses segments.
                result.subresources.push_back({.offset = segments.front().address,
                    .size = level.guestSize, .width = level.width, .height = level.height,
                    .depth = level.depth, .mip = static_cast<std::uint32_t>(mip), .layer = layer,
                    .segments = std::move(segments),
                    .blockHeight = level.blockHeight, .blockDepth = level.blockDepth});
                levelOffset += level.guestSize;
            }
        }
        return result;
    }

    struct ResolvedViewBase { std::uint32_t mip{}, layer{}; };

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

    struct ClassifiedResourceView {
        TextureViewCompatibility relation{};
        std::optional<ResolvedViewBase> sharedView{};
    };

    inline ClassifiedResourceView ClassifyAndResolveView(const TextureResourceLayout &backing,
                                                          const TextureResourceLayout &requested,
                                                          FormatCompatibility format, bool supportsFormatView) {
        auto relation = ClassifyTextureViewCompatibility(backing, requested, format, supportsFormatView);
        if (relation != TextureViewCompatibility::Full)
            return {relation, std::nullopt};
        auto resolved = ResolveFullView(backing, requested);
        return resolved ? ClassifiedResourceView{relation, resolved}
                        : ClassifiedResourceView{TextureViewCompatibility::LayoutIncompatible, std::nullopt};
    }
}
