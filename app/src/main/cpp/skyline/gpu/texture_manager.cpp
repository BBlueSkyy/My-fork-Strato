// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include <limits>
#include <gpu.h>
#include "texture/compatibility.h"
#include "texture/layout.h"
#include "texture/resource_layout.h"
#include "texture_manager.h"

namespace skyline::gpu {
    namespace {
        std::optional<texture::ImageKind> ImageKindOf(vk::ImageType type) {
            switch (type) {
                case vk::ImageType::e1D: return texture::ImageKind::OneDimensional;
                case vk::ImageType::e2D: return texture::ImageKind::TwoDimensional;
                case vk::ImageType::e3D: return texture::ImageKind::ThreeDimensional;
            }
            return std::nullopt;
        }

        std::optional<texture::ViewKind> ViewKindOf(vk::ImageViewType type) {
            switch (type) {
                case vk::ImageViewType::e1D: return texture::ViewKind::OneDimensional;
                case vk::ImageViewType::e1DArray: return texture::ViewKind::OneDimensionalArray;
                case vk::ImageViewType::e2D: return texture::ViewKind::TwoDimensional;
                case vk::ImageViewType::e2DArray: return texture::ViewKind::TwoDimensionalArray;
                case vk::ImageViewType::eCube: return texture::ViewKind::Cube;
                case vk::ImageViewType::eCubeArray: return texture::ViewKind::CubeArray;
                case vk::ImageViewType::e3D: return texture::ViewKind::ThreeDimensional;
            }
            return std::nullopt;
        }

        std::optional<texture::OwnedTextureResourceLayout> DescribeGuestLayout(
            const GuestTexture &guest, const texture::GuestResourceRanges &ranges, const Texture *backing = nullptr) {
            // A nonzero baseArrayLayer currently changes the translated mapping length;
            // its origin relative to the VkImage is not recorded in GuestTexture.
            if (!guest.format || guest.baseArrayLayer || !guest.mipLevelCount ||
                !guest.layerCount || !guest.viewMipCount)
                return std::nullopt;
            auto imageKind = ImageKindOf(guest.GetImageType());
            auto viewKind = ViewKindOf(guest.viewType);
            if (!imageKind || !viewKind)
                return std::nullopt;

            texture::TileLayout tile{};
            switch (guest.tileConfig.mode) {
                case texture::TileMode::Linear: tile.mode = texture::TileKind::Linear; break;
                case texture::TileMode::Pitch:
                    tile.mode = texture::TileKind::Pitch;
                    tile.pitch = guest.tileConfig.pitch;
                    break;
                case texture::TileMode::Block:
                    tile.mode = texture::TileKind::Block;
                    tile.blockHeight = guest.tileConfig.blockHeight;
                    tile.blockDepth = guest.tileConfig.blockDepth;
                    if (!tile.blockHeight || !tile.blockDepth)
                        return std::nullopt;
                    break;
            }

            std::vector<texture::MipDescription> mips;
            if (tile.mode == texture::TileKind::Block) {
                auto calculated = backing ? std::vector<texture::MipLevelLayout>{} :
                    texture::GetBlockLinearMipLayout(guest.dimensions,
                        guest.format->blockHeight, guest.format->blockWidth, guest.format->bpb,
                        guest.format->blockHeight, guest.format->blockWidth, guest.format->bpb,
                        guest.tileConfig.blockHeight, guest.tileConfig.blockDepth, guest.mipLevelCount);
                const auto &levels = backing ? backing->mipLayouts : calculated;
                if (levels.size() != guest.mipLevelCount)
                    return std::nullopt;
                for (const auto &level : levels) {
                    if (level.blockHeight > std::numeric_limits<u32>::max() ||
                        level.blockDepth > std::numeric_limits<u32>::max())
                        return std::nullopt;
                    mips.push_back({level.dimensions.width, level.dimensions.height,
                        level.dimensions.depth, level.blockLinearSize,
                        static_cast<u32>(level.blockHeight), static_cast<u32>(level.blockDepth)});
                }
            } else {
                // The existing upload path does not support linear/pitch mip chains.
                if (guest.mipLevelCount != 1)
                    return std::nullopt;
                mips.push_back({guest.dimensions.width, guest.dimensions.height,
                    guest.dimensions.depth, guest.CalculateLayerSize()});
            }

            const auto layerStride = guest.layerStride ? guest.layerStride : guest.CalculateLayerSize();
            texture::TextureResourceLayout info{
                .tile = tile,
                .imageType = *imageKind,
                .viewType = *viewKind,
                .cubeCompatible = backing && static_cast<bool>(backing->flags & vk::ImageCreateFlagBits::eCubeCompatible),
                .layerStride = layerStride,
                .viewMipBase = guest.viewMipBase,
                .viewMipCount = guest.viewMipCount,
                .viewLayerBase = guest.baseArrayLayer,
                .viewLayerCount = guest.GetViewLayerCount(),
            };
            return texture::BuildResourceLayout(ranges, mips, guest.layerCount, info);
        }

        // Restrict the new path to known Vulkan compatibility classes and image flags.
        bool VerifiedFormatClass(vk::Format lhs, vk::Format rhs) {
            const auto rgba = [](vk::Format format) {
                return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
            };
            const auto bgra = [](vk::Format format) {
                return format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
            };
            return (rgba(lhs) && rgba(rhs)) || (bgra(lhs) && bgra(rhs));
        }

        bool SupportsHostFormatView(const GPU &gpu, const Texture &backing, const GuestTexture &requested) {
            const auto viewFormat = requested.format == backing.guest->format
                ? backing.format->vkFormat : requested.format->vkFormat;
            if (viewFormat == backing.format->vkFormat)
                return true;
            if (!(backing.flags & vk::ImageCreateFlagBits::eMutableFormat) ||
                !VerifiedFormatClass(backing.format->vkFormat, viewFormat) ||
                requested.format->vkAspect != backing.format->vkAspect)
                return false;

            auto properties = gpu.vkPhysicalDevice.getFormatProperties(viewFormat);
            auto required = vk::FormatFeatureFlags{vk::FormatFeatureFlagBits::eSampledImage};
            if (backing.usage & vk::ImageUsageFlagBits::eColorAttachment)
                required |= vk::FormatFeatureFlagBits::eColorAttachment;
            return (properties.optimalTilingFeatures & required) == required;
        }
    }

    TextureManager::TextureManager(GPU &gpu) : gpu(gpu) {}

    std::shared_ptr<TextureView> TextureManager::FindOrCreate(const GuestTexture &guestTexture, ContextTag tag) {
        TRACE_EVENT("gpu", "TextureManager::FindOrCreate");

        texture::GuestResourceRanges guestRanges{guestTexture.mappings};
        if (!guestRanges.Valid())
            throw exception("Invalid guest texture mapping ranges");

        // A proven Full view can reuse the backing. Every other result must still
        // pass through the original lookup and synchronization decisions below.

        boost::container::small_vector<std::shared_ptr<Texture>, 4> matches{};
        auto mappingLookup{mappingCache.Lookup(guestRanges)};
        boost::container::small_vector<std::shared_ptr<texture::TextureStorage>, 4> visitedStorages{};

        const auto legacyExactMatch = [&guestTexture](const GuestTexture &existing, texture::FormatCompatibility format) {
            return texture::CanShareStorage(format) &&
                (((existing.dimensions.width == guestTexture.dimensions.width &&
                    existing.dimensions.height == guestTexture.dimensions.height) ||
                    existing.CalculateLayerSize() == guestTexture.CalculateLayerSize()) &&
                    existing.GetViewDepth() <= guestTexture.GetViewDepth() || existing.viewMipBase > 0) &&
                existing.tileConfig == guestTexture.tileConfig;
        };

        const auto requestedLayout = DescribeGuestLayout(guestTexture, guestRanges);
        std::shared_ptr<texture::TextureStorage> sharedStorage;
        texture::ResolvedViewBase sharedBase{};
        bool sharedExact{};

        if (requestedLayout) {
            for (const auto &storage : mappingLookup.storages) {
                if (!storage || storage->texture->replaced || !storage->texture->guest)
                    continue;
                const auto backingLayout = DescribeGuestLayout(*storage->texture->guest,
                    storage->ranges, storage->texture.get());
                if (!backingLayout)
                    continue; // Unresolved layout keeps the old lookup behavior.

                const auto format = texture::ClassifyFormatCompatibility(
                    *storage->texture->guest->format, *guestTexture.format);
                const auto relation = texture::ClassifyAndResolveView(backingLayout->Layout(),
                    requestedLayout->Layout(), format, SupportsHostFormatView(gpu, *storage->texture, guestTexture));
                switch (relation.relation) {
                    case texture::TextureViewCompatibility::Full: {
                        const auto base = *relation.sharedView;
                        const auto &image = *storage->texture;
                        // Matching selected mips is insufficient if the rest of the
                        // requested guest resource maps different physical bytes.
                        if (!storage->ranges.FindContainedOffset(guestRanges))
                            break;
                        if (base.mip >= image.levelCount || guestTexture.viewMipCount > image.levelCount - base.mip ||
                            base.layer >= image.layerCount || guestTexture.GetViewLayerCount() > image.layerCount - base.layer)
                            break;
                        const auto exact = storage->ranges.Size() == guestRanges.Size() &&
                            storage->ranges.FindContainedOffset(guestRanges) == 0;
                        if (!sharedStorage || (exact && !sharedExact) ||
                            (exact == sharedExact && (image.levelCount > sharedStorage->texture->levelCount ||
                                image.layerCount > sharedStorage->texture->layerCount))) {
                            sharedStorage = storage;
                            sharedBase = base;
                            sharedExact = exact;
                        }
                        break;
                    }
                    case texture::TextureViewCompatibility::CopyOnly:
                        // A separate representation needs explicit copy dependencies in the next phase.
                        break;
                    case texture::TextureViewCompatibility::LayoutIncompatible:
                    case texture::TextureViewCompatibility::Incompatible:
                        break;
                }
            }
        }

        if (sharedStorage) {
            ContextLock textureLock{tag, *sharedStorage->texture};
            return sharedStorage->texture->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
                .aspectMask = guestTexture.aspect,
                .baseMipLevel = sharedBase.mip,
                .levelCount = guestTexture.viewMipCount,
                .baseArrayLayer = sharedBase.layer,
                .layerCount = guestTexture.GetViewLayerCount(),
            }, guestTexture.format, guestTexture.swizzle);
        }

        std::shared_ptr<Texture> fullMatch{};
        std::shared_ptr<Texture> layerMipMatch{};
        std::shared_ptr<Texture> depthSliceMatch{};
        u32 matchLevel{};
        u32 matchLayer{};
        u32 depthSliceLevel{};
        u32 depthSlice{};
        u32 depthSliceParentDepth{};

        for (const auto *hostMapping : mappingLookup.firstMappingOverlaps) {
            auto &candidateStorage{hostMapping->storage};
            if (std::find(visitedStorages.begin(), visitedStorages.end(), candidateStorage) != visitedStorages.end())
                continue;
            visitedStorages.push_back(candidateStorage);
            if (candidateStorage->texture->replaced)
                continue;

            auto matchedOffset{candidateStorage->ranges.FindContainedOffset(guestRanges)};
            if (!matchedOffset)
                continue;

            if (*matchedOffset == 0 && candidateStorage->ranges.Size() == guestRanges.Size()) {
                // An exact physical match, including all spans in their logical order.
                auto &matchGuestTexture{*candidateStorage->texture->guest};
                auto formatCompatibility{texture::ClassifyFormatCompatibility(*matchGuestTexture.format, *guestTexture.format)};
                if (legacyExactMatch(matchGuestTexture, formatCompatibility)) {
                    fullMatch = candidateStorage->texture;
                } else {
                    matches.push_back(candidateStorage->texture);
                }
            } else {
                auto &matchGuestTexture{*candidateStorage->texture->guest};
                auto formatCompatibility{texture::ClassifyFormatCompatibility(*matchGuestTexture.format, *guestTexture.format)};

                // A render target may describe an individual Z slice of a block-linear
                // 3D texture. Its guest address starts inside the parent mip rather than
                // at the mip boundary, so the legacy mip/layer matcher below cannot find
                // it. Resolve the exact block-linear slice origin and reuse the existing
                // 3D backing through its 2D-array-compatible Vulkan view.
                if (matchGuestTexture.GetImageType() == vk::ImageType::e3D &&
                    matchGuestTexture.layerCount == 1 &&
                    guestTexture.layerCount == 1 &&
                    (guestTexture.viewType == vk::ImageViewType::e2D ||
                     guestTexture.viewType == vk::ImageViewType::e2DArray) &&
                    guestTexture.baseArrayLayer == 0 &&
                    guestTexture.mipLevelCount == 1 &&
                    guestTexture.viewMipBase == 0 &&
                    guestTexture.viewMipCount == 1 &&
                    texture::CanShareStorage(formatCompatibility) &&
                    matchGuestTexture.tileConfig.mode == texture::TileMode::Block &&
                    guestTexture.tileConfig.mode == texture::TileMode::Block) {
                    size_t memOffset{};
                    for (auto it{hostMappings.begin()}; it != hostMapping->iterator; ++it)
                        memOffset += it->size();
                    memOffset += static_cast<size_t>(guestMapping.data() - hostMapping->iterator->data());

                    size_t levelMemOffset{};
                    u32 level{};
                    for (const auto &mipLevel : hostMapping->texture->mipLayouts) {
                        if (mipLevel.dimensions.width == guestTexture.dimensions.width &&
                            mipLevel.dimensions.height == guestTexture.dimensions.height &&
                            mipLevel.blockHeight == guestTexture.tileConfig.blockHeight &&
                            mipLevel.blockDepth == guestTexture.tileConfig.blockDepth) {
                            const u32 viewLayerCount{guestTexture.GetViewLayerCount()};
                            for (u32 slice{}; slice < mipLevel.dimensions.depth; ++slice) {
                                auto sliceOffset{texture::GetBlockLinearDepthSliceOffset(
                                    mipLevel.dimensions,
                                    matchGuestTexture.format->blockWidth,
                                    matchGuestTexture.format->blockHeight,
                                    matchGuestTexture.format->bpb,
                                    mipLevel.blockHeight,
                                    mipLevel.blockDepth,
                                    slice
                                )};
                                if (sliceOffset && levelMemOffset + *sliceOffset == memOffset &&
                                    viewLayerCount && viewLayerCount <= mipLevel.dimensions.depth - slice) {
                                    if (!depthSliceMatch || mipLevel.dimensions.depth > depthSliceParentDepth) {
                                        depthSliceMatch = hostMapping->texture;
                                        depthSliceLevel = level;
                                        depthSlice = slice;
                                        depthSliceParentDepth = mipLevel.dimensions.depth;
                                    }
                                    break;
                                }
                            }
                        }

                        levelMemOffset += mipLevel.blockLinearSize;
                        ++level;
                    }

                }

                if (texture::CanShareStorage(formatCompatibility) && matchGuestTexture.tileConfig == guestTexture.tileConfig &&
                        (!layerMipMatch || (matchGuestTexture.GetViewLayerCount() >= layerMipMatch->guest->GetViewLayerCount() && matchGuestTexture.mipLevelCount >= layerMipMatch->guest->mipLevelCount))) {
                    boost::container::small_vector<size_t, 16> mipSizes{};
                    for (const auto &level : candidateStorage->texture->mipLayouts)
                        mipSizes.push_back(level.blockLinearSize);
                    const auto subresource{texture::LocateSubresource(*matchedOffset, matchGuestTexture.GetLayerStride(),
                        std::span<const size_t>{mipSizes.data(), mipSizes.size()}, candidateStorage->texture->layerCount)};

                    if (subresource && subresource->offsetWithinMip == 0 &&
                        mipSizes[subresource->mip] == guestTexture.CalculateLayerSize()) {
                        if (layerMipMatch)
                            layerMipMatch->replaced = true;

                        if (fullMatch)
                            fullMatch->replaced = true;

                        layerMipMatch = candidateStorage->texture;
                        matchLayer = static_cast<u32>(subresource->layer);
                        matchLevel = static_cast<u32>(subresource->mip);
                    }
                }
            }
         }

        if (depthSliceMatch) {
            // Prefer the real 3D storage over an independently cached slice. The latter
            // would otherwise retain stale contents after the guest renders into another
            // view of the same 3D resource.
            if (fullMatch && fullMatch != depthSliceMatch)
                fullMatch->replaced = true;

            ContextLock textureLock{tag, *depthSliceMatch};
            return depthSliceMatch->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
                .aspectMask = guestTexture.aspect,
                .baseMipLevel = depthSliceLevel,
                .levelCount = 1,
                .baseArrayLayer = depthSlice,
                .layerCount = guestTexture.GetViewLayerCount(),
            }, guestTexture.format, guestTexture.swizzle);
        } else if (layerMipMatch) {
            ContextLock textureLock{tag, *layerMipMatch};
            return layerMipMatch->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
                .aspectMask = guestTexture.aspect,
                .baseMipLevel = guestTexture.viewMipBase + matchLevel,
                .levelCount = guestTexture.viewMipCount,
                .baseArrayLayer = guestTexture.baseArrayLayer + matchLayer,
                .layerCount = guestTexture.GetViewLayerCount(),
            }, guestTexture.format, guestTexture.swizzle);
        } else if (fullMatch) {
            ContextLock textureLock{tag, *fullMatch};
            return fullMatch->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
                .aspectMask = guestTexture.aspect,
                .baseMipLevel = guestTexture.viewMipBase,
                .levelCount = guestTexture.viewMipCount,
                .baseArrayLayer = guestTexture.baseArrayLayer,
                .layerCount = guestTexture.GetViewLayerCount(),
            }, guestTexture.format, guestTexture.swizzle);
        }

        if (guestTexture.GetImageType() == vk::ImageType::e3D &&
            guestTexture.tileConfig.mode == texture::TileMode::Block) {
            auto parentMipLayouts{texture::GetBlockLinearMipLayout(
                guestTexture.dimensions,
                guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth,
                guestTexture.mipLevelCount
            )};

            auto getGuestOffset{[&](const u8 *address) -> std::optional<size_t> {
                auto addressValue{reinterpret_cast<uintptr_t>(address)};
                size_t mappingBase{};
                for (const auto &mapping : guestTexture.mappings) {
                    auto begin{reinterpret_cast<uintptr_t>(mapping.data())};
                    auto end{begin + mapping.size()};
                    if (addressValue >= begin && addressValue < end)
                        return mappingBase + static_cast<size_t>(addressValue - begin);
                    mappingBase += mapping.size();
                }
                return std::nullopt;
            }};

            struct SliceSource {
                std::shared_ptr<Texture> texture;
                u32 level;
                u32 slice;
            };
            boost::container::small_vector<SliceSource, 16> sliceSources;

            for (const auto &mapping : textures) {
                auto source{mapping.texture};
                if (!source || source->replaced || !source->guest)
                    continue;

                if (std::find_if(sliceSources.begin(), sliceSources.end(), [&](const SliceSource &entry) {
                        return entry.texture == source;
                    }) != sliceSources.end())
                    continue;

                const auto &sourceGuest{*source->guest};
                if (sourceGuest.GetImageType() != vk::ImageType::e2D ||
                    sourceGuest.dimensions.depth != 1 ||
                    sourceGuest.layerCount != 1 ||
                    sourceGuest.mipLevelCount != 1 ||
                    sourceGuest.baseArrayLayer != 0 ||
                    sourceGuest.format != guestTexture.format ||
                    sourceGuest.tileConfig.mode != texture::TileMode::Block ||
                    sourceGuest.tileConfig.blockDepth != guestTexture.tileConfig.blockDepth)
                    continue;

                auto sourceOffset{getGuestOffset(sourceGuest.mappings.front().data())};
                if (!sourceOffset)
                    continue;

                size_t levelOffset{};
                bool found{};
                for (u32 level{}; level < parentMipLayouts.size() && !found; ++level) {
                    const auto &mip{parentMipLayouts[level]};
                    if (sourceGuest.dimensions.width == mip.dimensions.width &&
                        sourceGuest.dimensions.height == mip.dimensions.height &&
                        sourceGuest.tileConfig.blockHeight == mip.blockHeight &&
                        sourceGuest.tileConfig.blockDepth == mip.blockDepth) {
                        for (u32 slice{}; slice < mip.dimensions.depth; ++slice) {
                            auto sliceOffset{texture::GetBlockLinearDepthSliceOffset(
                                mip.dimensions,
                                guestTexture.format->blockWidth,
                                guestTexture.format->blockHeight,
                                guestTexture.format->bpb,
                                mip.blockHeight,
                                mip.blockDepth,
                                slice
                            )};

                            if (sliceOffset && levelOffset + *sliceOffset == *sourceOffset) {
                                auto existing{std::find_if(sliceSources.begin(), sliceSources.end(),
                                    [level, slice](const SliceSource &entry) {
                                        return entry.level == level && entry.slice == slice;
                                    })};

                                if (existing == sliceSources.end()) {
                                    sliceSources.push_back({source, level, slice});
                                } else if (!existing->texture->everUsedAsRt && source->everUsedAsRt) {
                                    existing->texture = source;
                                }

                                found = true;
                                break;
                            }
                        }
                    }

                    levelOffset += mip.blockLinearSize;
                }
            }

            for (auto &source : sliceSources) {
                if (std::find(matches.begin(), matches.end(), source.texture) == matches.end())
                    matches.push_back(source.texture);
            }
        }

        for (auto &texture : matches)
            texture->SynchronizeGuest(false, true);

        // Create a texture as we cannot find one that matches


        auto texture{std::make_shared<Texture>(gpu, guestTexture)};
        texture->SetupGuestMappings();
        texture->TransitionLayout(vk::ImageLayout::eGeneral);
        auto storage{texture::CreateTextureStorage(texture, std::move(guestRanges))};
        texture::JoinTextureStorageGroups(storage, mappingLookup.storages);
        mappingCache.Insert(storage, storage->ranges);

        return texture->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
            .aspectMask = guestTexture.aspect,
            .baseMipLevel = guestTexture.viewMipBase,
            .levelCount = guestTexture.viewMipCount,
            .baseArrayLayer = guestTexture.baseArrayLayer,
            .layerCount = guestTexture.GetViewLayerCount(),
        }, guestTexture.format, guestTexture.swizzle);
    }
}
