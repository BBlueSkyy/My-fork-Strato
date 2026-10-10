// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <gpu.h>
#include "texture/compatibility.h"
#include "texture/layout.h"
#include "texture/resource_layout.h"
#include "texture_manager.h"

namespace skyline::gpu {
    namespace {
        template<typename Handle>
        std::uint64_t HandleValue(Handle handle) {
            if constexpr (std::is_pointer_v<Handle>)
                return reinterpret_cast<std::uintptr_t>(handle);
            else
                return static_cast<std::uint64_t>(handle);
        }

        std::uint64_t ImageHandleValue(vk::Image image) {
            return HandleValue(static_cast<VkImage>(image));
        }

        const char *ImageKindName(texture::ImageKind kind) {
            switch (kind) {
                case texture::ImageKind::OneDimensional: return "1D";
                case texture::ImageKind::TwoDimensional: return "2D";
                case texture::ImageKind::ThreeDimensional: return "3D";
            }
            return "?";
        }

        const char *CopyStateName(
            const std::optional<texture::CopyRepresentationDebugInfo> &info) {
            if (!info)
                return "Untracked";
            switch (info->state) {
                case texture::CopyRepresentationState::Untracked: return "Untracked";
                case texture::CopyRepresentationState::Current: return "Current";
                case texture::CopyRepresentationState::Stale: return "Stale";
            }
            return "?";
        }

        texture::Dimensions ActualMipDimensions(const Texture &image, std::uint32_t mip) {
            if (mip >= image.levelCount || mip >= image.mipLayouts.size())
                return {};
            return image.mipLayouts[mip].dimensions;
        }

        void LogDepthSliceRouteRegistration(
            const std::shared_ptr<texture::TextureGroup> &group,
            const std::shared_ptr<texture::TextureStorage> &source,
            const texture::TextureResourceLayout &sourceLayout,
            texture::ResolvedSubresource sourceSubresource,
            const std::shared_ptr<texture::TextureStorage> &destination,
            const texture::TextureResourceLayout &destinationLayout,
            texture::ResolvedSubresource destinationSubresource) {
            if (!group || !source || !source->texture || !destination || !destination->texture)
                return;
            const auto sourceInfo{
                group->GetCopyRepresentationDebugInfo(source, sourceSubresource)};
            const auto destinationInfo{
                group->GetCopyRepresentationDebugInfo(destination, destinationSubresource)};
            const auto sourceMip{ActualMipDimensions(*source->texture, sourceSubresource.mip)};
            const auto destinationMip{
                ActualMipDimensions(*destination->texture, destinationSubresource.mip)};
            const auto sourceDescription{
                texture::FindSubresource(sourceLayout, sourceSubresource)};
            const auto width{sourceDescription ? sourceDescription->width : 0};
            const auto height{sourceDescription ? sourceDescription->height : 0};
            const auto sourceOffsetZ{sourceLayout.imageType == texture::ImageKind::ThreeDimensional
                ? sourceSubresource.depthSlice : 0};
            const auto destinationOffsetZ{
                destinationLayout.imageType == texture::ImageKind::ThreeDimensional
                    ? destinationSubresource.depthSlice : 0};

            LOGI("TexmanDepthCopy register {}->{} srcStorage={} srcImage=0x{:X} "
                 "srcSub={}/{}/{} srcMip={}x{}x{} srcZ={} srcLayout={} srcState={} "
                 "srcGen={}/{} dstStorage={} dstImage=0x{:X} dstSub={}/{}/{} "
                 "dstMip={}x{}x{} dstZ={} dstLayout={} dstState={} dstGen={}/{} "
                 "extent={}x{}x1",
                 ImageKindName(sourceLayout.imageType), ImageKindName(destinationLayout.imageType),
                 fmt::ptr(source.get()), ImageHandleValue(source->texture->GetBacking()),
                 sourceSubresource.mip, sourceSubresource.layer, sourceSubresource.depthSlice,
                 sourceMip.width, sourceMip.height, sourceMip.depth, sourceOffsetZ,
                 vk::to_string(source->texture->layout), CopyStateName(sourceInfo),
                 sourceInfo ? sourceInfo->generation : 0,
                 sourceInfo ? sourceInfo->currentGeneration : 0,
                 fmt::ptr(destination.get()), ImageHandleValue(destination->texture->GetBacking()),
                 destinationSubresource.mip, destinationSubresource.layer,
                 destinationSubresource.depthSlice,
                 destinationMip.width, destinationMip.height, destinationMip.depth,
                 destinationOffsetZ, vk::to_string(destination->texture->layout),
                 CopyStateName(destinationInfo),
                 destinationInfo ? destinationInfo->generation : 0,
                 destinationInfo ? destinationInfo->currentGeneration : 0,
                 width, height);
        }

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
                .formatBlockWidth = static_cast<u32>(guest.format->blockWidth),
                .formatBlockHeight = static_cast<u32>(guest.format->blockHeight),
                .formatBytesPerBlock = static_cast<u32>(guest.format->bpb),
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

        texture::CopyImageInfo DescribeCopyImage(const Texture &texture) {
            return {
                .hostFormat = static_cast<std::uint64_t>(
                    static_cast<VkFormat>(texture.format->vkFormat)),
                .aspectMask = static_cast<std::uint32_t>(
                    static_cast<VkImageAspectFlags>(texture.format->vkAspect)),
                .sampleCount = static_cast<std::uint32_t>(
                    static_cast<VkSampleCountFlagBits>(texture.sampleCount)),
                .transferSource = static_cast<bool>(
                    texture.usage & vk::ImageUsageFlagBits::eTransferSrc),
                .transferDestination = static_cast<bool>(
                    texture.usage & vk::ImageUsageFlagBits::eTransferDst),
            };
        }

        bool IsMaintenance5DimensionalPair(const texture::TextureResourceLayout &first,
                                           const texture::TextureResourceLayout &second) {
            return (first.imageType == texture::ImageKind::OneDimensional &&
                    second.imageType == texture::ImageKind::TwoDimensional) ||
                (first.imageType == texture::ImageKind::TwoDimensional &&
                 second.imageType == texture::ImageKind::OneDimensional);
        }

        bool IsDepthSliceCopy(const texture::TextureResourceLayout &backing,
                              const texture::TextureResourceLayout &requested,
                              const texture::ClassifiedResourceView &classified) {
            if (classified.relation != texture::TextureViewCompatibility::CopyOnly ||
                !classified.copyRegion || classified.copyRegion->subresources.size() != 1)
                return false;
            const auto &mapping{classified.copyRegion->subresources.front()};
            return texture::IsExactBlockLinearDepthSliceRelation(
                backing, mapping.backing, requested, mapping.requested);
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

        struct ClassifiedStorage {
            std::shared_ptr<texture::TextureStorage> storage;
            texture::ClassifiedResourceView view;
            texture::OwnedTextureResourceLayout layout;
            texture::FormatCompatibility format;
            bool depthSliceCopy{};
        };

        const auto requestedLayout = DescribeGuestLayout(guestTexture, guestRanges);
        boost::container::small_vector<ClassifiedStorage, 4> classifiedStorages{};

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
                const bool depthSliceCopy{IsDepthSliceCopy(
                    backingLayout->Layout(), requestedLayout->Layout(), relation)};
                classifiedStorages.push_back({
                    storage, relation, std::move(*backingLayout), format, depthSliceCopy});
            }
        }

        const auto classifiedStorage = [&classifiedStorages](const std::shared_ptr<texture::TextureStorage> &storage)
                -> const ClassifiedStorage * {
            const auto entry = std::find_if(classifiedStorages.begin(), classifiedStorages.end(),
                [&storage](const ClassifiedStorage &candidate) { return candidate.storage == storage; });
            return entry == classifiedStorages.end() ? nullptr : &*entry;
        };
        const auto classifiedView = [&classifiedStorage](const std::shared_ptr<texture::TextureStorage> &storage)
                -> const texture::ClassifiedResourceView * {
            const auto classified{classifiedStorage(storage)};
            return classified ? &classified->view : nullptr;
        };

        std::shared_ptr<Texture> fullMatch{};
        std::shared_ptr<texture::TextureStorage> fullMatchStorage{};
        std::shared_ptr<Texture> layerMipMatch{};
        std::shared_ptr<Texture> depthSliceMatch{};
        bool depthSliceCopyMatch{};
        std::shared_ptr<texture::TextureStorage> layerMipMatchStorage{};
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
                    fullMatchStorage = candidateStorage;
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
                    const size_t memOffset{*matchedOffset};

                    size_t levelMemOffset{};
                    u32 level{};
                    for (const auto &mipLevel : candidateStorage->texture->mipLayouts) {
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
                                        depthSliceMatch = candidateStorage->texture;
                                        depthSliceLevel = level;
                                        depthSlice = slice;
                                        depthSliceParentDepth = mipLevel.dimensions.depth;
                                        const auto classified{classifiedStorage(candidateStorage)};
                                        depthSliceCopyMatch = classified && classified->depthSliceCopy;
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
                        layerMipMatchStorage = candidateStorage;
                        matchLayer = static_cast<u32>(subresource->layer);
                        matchLevel = static_cast<u32>(subresource->mip);
                    }
                }
            }
         }

        const auto legacyDepthSliceView = [&]() {
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
        };

        if (depthSliceMatch && !depthSliceCopyMatch) {
            return legacyDepthSliceView();
        } else if (layerMipMatch) {
            ContextLock textureLock{tag, *layerMipMatch};
            const texture::ResolvedViewBase legacyBase{
                .mip = guestTexture.viewMipBase + matchLevel,
                .layer = guestTexture.baseArrayLayer + matchLayer,
            };
            const auto classified = classifiedView(layerMipMatchStorage);
            const auto sharedBase = classified
                ? texture::ConfirmFullViewAgainstLegacy(*classified, legacyBase)
                : std::nullopt;
            return layerMipMatch->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
                .aspectMask = guestTexture.aspect,
                .baseMipLevel = sharedBase ? sharedBase->mip : legacyBase.mip,
                .levelCount = guestTexture.viewMipCount,
                .baseArrayLayer = sharedBase ? sharedBase->layer : legacyBase.layer,
                .layerCount = guestTexture.GetViewLayerCount(),
            }, guestTexture.format, guestTexture.swizzle);
        } else if (fullMatch) {
            ContextLock textureLock{tag, *fullMatch};
            const texture::ResolvedViewBase legacyBase{
                .mip = guestTexture.viewMipBase,
                .layer = guestTexture.baseArrayLayer,
            };
            const auto classified = classifiedView(fullMatchStorage);
            const auto sharedBase = classified
                ? texture::ConfirmFullViewAgainstLegacy(*classified, legacyBase)
                : std::nullopt;
            return fullMatch->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
                .aspectMask = guestTexture.aspect,
                .baseMipLevel = sharedBase ? sharedBase->mip : legacyBase.mip,
                .levelCount = guestTexture.viewMipCount,
                .baseArrayLayer = sharedBase ? sharedBase->layer : legacyBase.layer,
                .layerCount = guestTexture.GetViewLayerCount(),
            }, guestTexture.format, guestTexture.swizzle);
        }

        if (depthSliceMatch &&
            std::find(matches.begin(), matches.end(), depthSliceMatch) == matches.end())
            matches.push_back(depthSliceMatch);

        if (guestTexture.GetImageType() == vk::ImageType::e3D &&
            guestTexture.tileConfig.mode == texture::TileMode::Block) {
            auto parentMipLayouts{texture::GetBlockLinearMipLayout(
                guestTexture.dimensions,
                guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth,
                guestTexture.mipLevelCount
            )};

            struct SliceSource {
                std::shared_ptr<Texture> texture;
                u32 level;
                u32 slice;
            };
            boost::container::small_vector<SliceSource, 16> sliceSources;

            for (const auto &sourceStorage : mappingCache.StoragesInMappingOrder()) {
                auto source{sourceStorage->texture};
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

                auto sourceOffset{guestRanges.FindUniqueOffset(sourceGuest.mappings.front().data())};
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

        if (requestedLayout) {
            const auto requestedImage{DescribeCopyImage(*texture)};
            const auto createdLayout{DescribeGuestLayout(guestTexture, storage->ranges, texture.get())};
            bool matchedDepthSliceRegistered{!depthSliceCopyMatch};
            for (const auto &classified : classifiedStorages) {
                if (!classified.storage ||
                    classified.view.relation != texture::TextureViewCompatibility::CopyOnly ||
                    !classified.view.copyRegion)
                    continue;

                const auto backingImage{DescribeCopyImage(*classified.storage->texture)};
                auto group{storage->GetGroup()};
                auto backingGroup{classified.storage->GetGroup()};
                if (!group)
                    continue;

                if (createdLayout && IsDepthSliceCopy(
                        classified.layout.Layout(), createdLayout->Layout(), classified.view)) {
                    if (group != backingGroup) {
                        if (group->HasExecutableCopyRoutes()) {
                            if (!backingGroup || !group->TryMergeCopyDependenciesFrom(*backingGroup))
                                continue;
                        } else {
                            group = backingGroup;
                        }
                    }
                    if (group && group->RegisterDepthSliceCopyOnly(
                            classified.storage, classified.layout.Layout(), backingImage,
                            storage, createdLayout->Layout(), requestedImage,
                            classified.view, classified.format)) {
                        const auto &mapping{
                            classified.view.copyRegion->subresources.front()};
                        LogDepthSliceRouteRegistration(
                            group, classified.storage, classified.layout.Layout(), mapping.backing,
                            storage, createdLayout->Layout(), mapping.requested);
                        LogDepthSliceRouteRegistration(
                            group, storage, createdLayout->Layout(), mapping.requested,
                            classified.storage, classified.layout.Layout(), mapping.backing);
                        if (depthSliceMatch && classified.storage->texture == depthSliceMatch)
                            matchedDepthSliceRegistered = true;
                    }
                    continue;
                }

                if (gpu.traits.supportsMaintenance5 &&
                    IsMaintenance5DimensionalPair(
                        classified.layout.Layout(), requestedLayout->Layout()))
                    group->RegisterMaintenance5CopyOnly(
                        classified.storage, classified.layout.Layout(), backingImage,
                        storage, requestedLayout->Layout(), requestedImage,
                        classified.view);

                if (createdLayout)
                    group->RegisterCubeCompatibleCopyOnly(
                        classified.storage, classified.layout.Layout(), backingImage,
                        storage, createdLayout->Layout(), requestedImage,
                        classified.view, classified.format);
            }

            if (!matchedDepthSliceRegistered)
                return legacyDepthSliceView();
        }

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
