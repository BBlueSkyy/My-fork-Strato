// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

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

    /** Physical guest address and layout for one mip and array layer. */
    struct GuestSubresource {
        std::uint64_t offset{};
        std::uint64_t size{};
        std::uint32_t width{}, height{}, depth{};
        std::uint32_t mip{}, layer{};
    };

    /**
     * Subresources use resolved physical guest addresses, not offsets in a virtual mapping.
     * Callers must resolve all mappings and the exact mip/layer offsets before classifying.
     * An unresolved or non-contiguous guest mapping must never be assumed contiguous here.
     */
    struct TextureResourceLayout {
        TileLayout tile{};
        ImageKind imageType{};
        ViewKind viewType{};
        bool cubeCompatible{}; //!< Backing VkImage was created with cube compatibility.
        std::uint64_t layerStride{};
        std::uint32_t viewMipBase{}, viewMipCount{};
        std::uint32_t viewLayerBase{}, viewLayerCount{};
        std::span<const GuestSubresource> subresources{};
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

    constexpr bool Overlaps(const GuestSubresource &lhs, const GuestSubresource &rhs) {
        // Subresource ranges are validated by the caller; avoid wrapping their end addresses.
        if (!lhs.size || !rhs.size ||
            lhs.size > std::numeric_limits<std::uint64_t>::max() - lhs.offset ||
            rhs.size > std::numeric_limits<std::uint64_t>::max() - rhs.offset)
            return false;
        return lhs.offset < rhs.offset + rhs.size && rhs.offset < lhs.offset + lhs.size;
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

        bool overlaps{};
        bool aligned{true};
        std::uint64_t selected{};
        for (std::size_t index{}; index < requested.subresources.size(); ++index) {
            const auto &subresource{requested.subresources[index]};
            if (subresource.mip < requested.viewMipBase ||
                subresource.mip - requested.viewMipBase >= requested.viewMipCount ||
                subresource.layer < requested.viewLayerBase ||
                subresource.layer - requested.viewLayerBase >= requested.viewLayerCount)
                continue;

            ++selected;
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
                if (subresource.offset == candidate.offset && subresource.size == candidate.size &&
                    subresource.width == candidate.width && subresource.height == candidate.height &&
                    subresource.depth == candidate.depth)
                    matched = true;
            }
            if (!matched)
                aligned = false;
        }

        if (!overlaps)
            return TextureViewCompatibility::Incompatible;

        if (selected != std::uint64_t{requested.viewMipCount} * requested.viewLayerCount ||
            !aligned || backing.tile != requested.tile ||
            backing.imageType != requested.imageType ||
            !ValidViewType(backing.imageType, backing.viewType) ||
            !ValidViewType(backing.imageType, requested.viewType) ||
            (format == FormatCompatibility::ViewCompatible && !supportsFormatView) ||
            ((requested.viewType == ViewKind::Cube || requested.viewType == ViewKind::CubeArray) &&
                (!backing.cubeCompatible || requested.viewLayerBase % 6 ||
                    (requested.viewType == ViewKind::Cube ? requested.viewLayerCount != 6 : requested.viewLayerCount % 6))) ||
            (requested.viewLayerCount > 1 && backing.layerStride != requested.layerStride))
            return TextureViewCompatibility::LayoutIncompatible;

        return format == FormatCompatibility::CopyCompatible
            ? TextureViewCompatibility::CopyOnly
            : TextureViewCompatibility::Full;
    }
}
