// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include "guest_range.h"

namespace skyline::gpu::texture {
    /** CopyCompatible is reserved for formats with a verified explicit conversion path. */
    enum class FormatCompatibility : std::uint8_t {
        Exact,
        ViewCompatible,
        CopyCompatible,
        Incompatible,
    };

    /** This describes a relationship between resources, independently of their format. */
    enum class TextureViewCompatibility : std::uint8_t {
        Full,               //!< A subresource view of the same host image is possible.
        CopyOnly,           //!< Matching subresources require distinct host representations and an explicit copy.
        LayoutIncompatible, //!< Guest bytes overlap but do not describe the same subresources.
        Incompatible,       //!< No overlapping subresources or an incompatible format.
    };

    enum class TileKind : std::uint8_t { Linear, Pitch, Block };
    enum class ImageKind : std::uint8_t { OneDimensional, TwoDimensional, ThreeDimensional };
    enum class ViewKind : std::uint8_t {
        OneDimensional, OneDimensionalArray, TwoDimensional, TwoDimensionalArray,
        Cube, CubeArray, ThreeDimensional,
    };

    struct TileLayout {
        TileKind mode{};
        std::uint32_t pitch{};
        std::uint32_t blockHeight{};
        std::uint32_t blockDepth{};

        constexpr bool operator==(const TileLayout &) const = default;
    };

    /** Physical GOB spans that contain the texels of one block-linear Z slice. */
    struct GuestDepthSlice {
        std::uint64_t size{};
        std::vector<GuestResourceRanges::Segment> segments{};
    };

    /** Physical guest spans and layout for one mip and array layer. */
    struct GuestSubresource {
        std::uint64_t offset{};
        std::uint64_t size{};
        std::uint32_t width{}, height{}, depth{};
        std::uint32_t mip{}, layer{};
        // Populated by real guest layouts. An empty list preserves the single-span
        // representation used by existing, contiguous compatibility fixtures.
        std::vector<GuestResourceRanges::Segment> segments{};
        std::uint32_t blockHeight{}, blockDepth{}; //!< Effective GOB blocks at this mip, when block-linear.
        std::vector<GuestDepthSlice> depthSlices{};
    };

    /**
     * Subresources use resolved physical guest spans, never a fabricated continuous address.
     * Callers must resolve all mappings and the exact mip/layer offsets before classifying.
     */
    struct TextureResourceLayout {
        TileLayout tile{};
        ImageKind imageType{};
        ViewKind viewType{};
        bool cubeCompatible{}; //!< Backing VkImage was created with cube compatibility.
        std::uint64_t layerStride{};
        std::uint32_t viewMipBase{}, viewMipCount{};
        std::uint32_t viewLayerBase{}, viewLayerCount{};
        std::uint32_t formatBlockWidth{}, formatBlockHeight{}, formatBytesPerBlock{};
        std::span<const GuestSubresource> subresources{};
    };

    struct GuestDepthSliceMatch {
        std::uint32_t backingMip{}, backingLayer{}, backingDepthSlice{};
        std::uint32_t requestedMip{}, requestedLayer{}, requestedDepthSlice{};
    };

    constexpr bool ValidViewType(ImageKind image, ViewKind view) {
        switch (image) {
            case ImageKind::OneDimensional:
                return view == ViewKind::OneDimensional || view == ViewKind::OneDimensionalArray;
            case ImageKind::TwoDimensional:
                return view == ViewKind::TwoDimensional || view == ViewKind::TwoDimensionalArray ||
                    view == ViewKind::Cube || view == ViewKind::CubeArray;
            case ImageKind::ThreeDimensional:
                return view == ViewKind::ThreeDimensional;
        }
        return false;
    }

    inline bool Overlaps(const GuestSubresource &lhs, const GuestSubresource &rhs) {
        const auto lhsSingle = GuestResourceRanges::Segment{lhs.offset, lhs.size, 0};
        const auto rhsSingle = GuestResourceRanges::Segment{rhs.offset, rhs.size, 0};
        const auto left = lhs.segments.empty() ? std::span{&lhsSingle, 1} : std::span{lhs.segments};
        const auto right = rhs.segments.empty() ? std::span{&rhsSingle, 1} : std::span{rhs.segments};
        for (const auto &a : left)
            for (const auto &b : right)
                if (a.size && b.size && a.size <= std::numeric_limits<std::uintptr_t>::max() - a.address &&
                    b.size <= std::numeric_limits<std::uintptr_t>::max() - b.address &&
                    a.address < b.address + b.size && b.address < a.address + a.size)
                    return true;
        return false;
    }

    inline bool SameGuestBytes(std::uint64_t lhsSize,
                               std::span<const GuestResourceRanges::Segment> left,
                               std::uint64_t rhsSize,
                               std::span<const GuestResourceRanges::Segment> right) {
        if (!lhsSize || lhsSize != rhsSize)
            return false;
        std::size_t li{}, ri{}, lo{}, ro{}, compared{};
        while (compared < lhsSize) {
            if (li >= left.size() || ri >= right.size() ||
                left[li].size > std::numeric_limits<std::uintptr_t>::max() - left[li].address ||
                right[ri].size > std::numeric_limits<std::uintptr_t>::max() - right[ri].address ||
                left[li].address + lo != right[ri].address + ro)
                return false;
            const auto amount = std::min<std::uint64_t>({left[li].size - lo, right[ri].size - ro, lhsSize - compared});
            if (!amount)
                return false;
            compared += amount;
            if ((lo += amount) == left[li].size) { ++li; lo = 0; }
            if ((ro += amount) == right[ri].size) { ++ri; ro = 0; }
        }
        return li == left.size() && ri == right.size();
    }

    inline bool SameGuestBytes(const GuestSubresource &lhs, const GuestSubresource &rhs) {
        const auto lhsSingle = GuestResourceRanges::Segment{lhs.offset, lhs.size, 0};
        const auto rhsSingle = GuestResourceRanges::Segment{rhs.offset, rhs.size, 0};
        const auto left = lhs.segments.empty() ? std::span{&lhsSingle, 1} : std::span{lhs.segments};
        const auto right = rhs.segments.empty() ? std::span{&rhsSingle, 1} : std::span{rhs.segments};
        return SameGuestBytes(lhs.size, left, rhs.size, right);
    }

    inline bool SelectedSubresource(const TextureResourceLayout &layout,
                                    const GuestSubresource &subresource) {
        return subresource.mip >= layout.viewMipBase &&
            subresource.mip - layout.viewMipBase < layout.viewMipCount &&
            subresource.layer >= layout.viewLayerBase &&
            subresource.layer - layout.viewLayerBase < layout.viewLayerCount;
    }

    inline std::optional<GuestDepthSliceMatch> FindExactBlockLinearDepthSlice(
        const TextureResourceLayout &backing, const TextureResourceLayout &requested) {
        const bool backing3D{backing.imageType == ImageKind::ThreeDimensional};
        const bool requested3D{requested.imageType == ImageKind::ThreeDimensional};
        if (backing3D == requested3D ||
            (backing.imageType != ImageKind::TwoDimensional && !backing3D) ||
            (requested.imageType != ImageKind::TwoDimensional && !requested3D) ||
            backing.tile.mode != TileKind::Block || requested.tile.mode != TileKind::Block ||
            !ValidViewType(backing.imageType, backing.viewType) ||
            !ValidViewType(requested.imageType, requested.viewType))
            return std::nullopt;

        const auto &volume{backing3D ? backing : requested};
        const auto &surface{backing3D ? requested : backing};
        if (volume.viewLayerBase || volume.viewLayerCount != 1 ||
            surface.viewMipCount != 1 || surface.viewLayerCount != 1)
            return std::nullopt;

        const GuestSubresource *surfaceSubresource{};
        for (const auto &candidate : surface.subresources) {
            if (!SelectedSubresource(surface, candidate))
                continue;
            if (surfaceSubresource)
                return std::nullopt;
            surfaceSubresource = &candidate;
        }
        if (!surfaceSubresource || surfaceSubresource->depth != 1 ||
            surfaceSubresource->depthSlices.size() != 1)
            return std::nullopt;

        std::optional<GuestDepthSliceMatch> result;
        for (const auto &candidate : volume.subresources) {
            if (!SelectedSubresource(volume, candidate) ||
                candidate.layer != 0 || candidate.depthSlices.size() != candidate.depth ||
                candidate.width != surfaceSubresource->width ||
                candidate.height != surfaceSubresource->height ||
                candidate.blockHeight != surfaceSubresource->blockHeight ||
                candidate.blockDepth != surfaceSubresource->blockDepth)
                continue;
            for (std::uint32_t slice{}; slice < candidate.depth; ++slice) {
                const auto &volumeSlice{candidate.depthSlices[slice]};
                const auto &surfaceSlice{surfaceSubresource->depthSlices.front()};
                if (!SameGuestBytes(volumeSlice.size, volumeSlice.segments,
                                    surfaceSlice.size, surfaceSlice.segments))
                    continue;
                if (result)
                    return std::nullopt;
                if (backing3D) {
                    result = GuestDepthSliceMatch{
                        candidate.mip, candidate.layer, slice,
                        surfaceSubresource->mip, surfaceSubresource->layer, 0,
                    };
                } else {
                    result = GuestDepthSliceMatch{
                        surfaceSubresource->mip, surfaceSubresource->layer, 0,
                        candidate.mip, candidate.layer, slice,
                    };
                }
            }
        }
        return result;
    }

    /** A mip zero descriptor must agree with its resource's initial GOB configuration. */
    inline bool ValidBaseBlock(const TextureResourceLayout &layout) {
        bool found{};
        for (const auto &subresource : layout.subresources) {
            if (subresource.mip != 0 || !subresource.blockHeight || !subresource.blockDepth)
                continue;
            found = true;
            if (subresource.blockHeight != layout.tile.blockHeight ||
                subresource.blockDepth != layout.tile.blockDepth)
                return false;
        }
        return found;
    }

    /**
     * Classify a requested view against the backing image's complete subresource layout.
     * This only reports possible relationships: it does not grant a Vulkan view, schedule a
     * copy, or determine which representation contains the latest GPU-written contents.
     * supportsFormatView must be verified for this particular pair of host formats and
     * backing image creation flags; guest texel compatibility alone is insufficient.
     */
    inline TextureViewCompatibility ClassifyTextureViewCompatibility(
        const TextureResourceLayout &backing, const TextureResourceLayout &requested,
        FormatCompatibility format, bool supportsFormatView = false) {
        if (format == FormatCompatibility::Incompatible ||
            !requested.viewMipCount || !requested.viewLayerCount ||
            requested.subresources.empty() || backing.subresources.empty())
            return TextureViewCompatibility::Incompatible;

        if (format == FormatCompatibility::Exact &&
            FindExactBlockLinearDepthSlice(backing, requested))
            return TextureViewCompatibility::CopyOnly;

        bool overlaps{};
        bool aligned{true};
        bool unitHeightDepth{true};
        bool unitDepth{true};
        bool squareExtents{true};
        bool effectiveBlockLayout{backing.tile.mode == TileKind::Block && requested.tile.mode == TileKind::Block};
        std::uint64_t selected{};
        for (std::size_t index{}; index < requested.subresources.size(); ++index) {
            const auto &subresource{requested.subresources[index]};
            if (subresource.mip < requested.viewMipBase ||
                subresource.mip - requested.viewMipBase >= requested.viewMipCount ||
                subresource.layer < requested.viewLayerBase ||
                subresource.layer - requested.viewLayerBase >= requested.viewLayerCount)
                continue;

            ++selected;
            if (subresource.height != 1 || subresource.depth != 1)
                unitHeightDepth = false;
            if (subresource.depth != 1)
                unitDepth = false;
            if (subresource.width != subresource.height)
                squareExtents = false;
            for (std::size_t previous{}; previous < index; ++previous) {
                const auto &other{requested.subresources[previous]};
                if (subresource.mip == other.mip && subresource.layer == other.layer)
                    aligned = false;
            }
            bool matched{};
            for (const auto &candidate : backing.subresources) {
                if (!Overlaps(subresource, candidate))
                    continue;
                overlaps = true;
                if (SameGuestBytes(subresource, candidate) &&
                    subresource.width == candidate.width && subresource.height == candidate.height &&
                    subresource.depth == candidate.depth) {
                    matched = true;
                    if (effectiveBlockLayout) {
                        if (!subresource.blockHeight || !subresource.blockDepth ||
                            !candidate.blockHeight || !candidate.blockDepth)
                            effectiveBlockLayout = false;
                        else if (subresource.blockHeight != candidate.blockHeight ||
                                 subresource.blockDepth != candidate.blockDepth)
                            aligned = false;
                    }
                }
            }
            if (!matched)
                aligned = false;
        }

        if (!overlaps)
            return TextureViewCompatibility::Incompatible;

        const bool sameTile = effectiveBlockLayout && ValidBaseBlock(backing) && ValidBaseBlock(requested)
            ? true : backing.tile == requested.tile;
        // Height-one 1D/2D aliases are semantically copy-related. Executing that relation
        // as an image copy still requires the appropriate Vulkan capability (maintenance5).
        const bool oneDimensionalCopy = format == FormatCompatibility::Exact && unitHeightDepth &&
            ((backing.imageType == ImageKind::OneDimensional && requested.imageType == ImageKind::TwoDimensional) ||
             (backing.imageType == ImageKind::TwoDimensional && requested.imageType == ImageKind::OneDimensional));
        const bool cubeView = requested.viewType == ViewKind::Cube ||
            requested.viewType == ViewKind::CubeArray;
        const bool validCubeLayers = requested.viewLayerBase % 6 == 0;
        const bool validCubeCount = requested.viewType == ViewKind::Cube
            ? requested.viewLayerCount == 6
            : requested.viewType != ViewKind::CubeArray || requested.viewLayerCount % 6 == 0;
        // This relation needs a distinct host image only because VkImageView creation
        // cannot add VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT to an existing 2D backing.
        const bool cubeCompatibleCopy = format == FormatCompatibility::Exact && unitDepth && squareExtents && cubeView &&
            validCubeLayers && validCubeCount && !backing.cubeCompatible &&
            backing.imageType == ImageKind::TwoDimensional &&
            requested.imageType == ImageKind::TwoDimensional;
        if (selected != std::uint64_t{requested.viewMipCount} * requested.viewLayerCount ||
            !aligned || !sameTile ||
            !ValidViewType(backing.imageType, backing.viewType) ||
            !ValidViewType(requested.imageType, requested.viewType) ||
            (backing.imageType != requested.imageType && !oneDimensionalCopy) ||
            (format == FormatCompatibility::ViewCompatible && !supportsFormatView) ||
            (cubeView && (!unitDepth || !squareExtents || !validCubeLayers || !validCubeCount ||
                (!backing.cubeCompatible && !cubeCompatibleCopy))) ||
            (requested.viewLayerCount > 1 && backing.layerStride != requested.layerStride))
            return TextureViewCompatibility::LayoutIncompatible;

        return oneDimensionalCopy || cubeCompatibleCopy || format == FormatCompatibility::CopyCompatible
            ? TextureViewCompatibility::CopyOnly
            : TextureViewCompatibility::Full;
    }
}
