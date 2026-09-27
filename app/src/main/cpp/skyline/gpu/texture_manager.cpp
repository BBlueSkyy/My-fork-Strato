// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cstdint>
#include <optional>
#include <common/trace.h>
#include "texture/layout.h"
#include "texture_manager.h"

namespace skyline::gpu {
    namespace {
        struct SliceLocation {
            u32 level;
            u32 slice;
        };

        // A block-linear 2D slice starts at a Z GOB within a 3D mip level. Its
        // advertised mapping includes the other Z GOBs, so span containment
        // alone cannot identify slices after the first one.
        std::optional<SliceLocation> Find3DSlice(const GuestTexture &volume, span<u8> volumeMapping,
                                                 const GuestTexture &slice, span<u8> sliceMapping) {
            if (volume.GetImageType() != vk::ImageType::e3D || slice.GetImageType() != vk::ImageType::e2D ||
                (slice.viewType != vk::ImageViewType::e2D && slice.viewType != vk::ImageViewType::e2DArray) ||
                volume.mappings.size() != 1 || slice.mappings.size() != 1 ||
                volume.tileConfig.mode != texture::TileMode::Block || slice.tileConfig.mode != texture::TileMode::Block ||
                slice.dimensions.depth != 1 || slice.layerCount != 1 || slice.baseArrayLayer != 0 ||
                slice.mipLevelCount != 1 || slice.viewMipBase != 0 || slice.viewMipCount != 1 ||
                volume.format != slice.format)
                return std::nullopt;

            const auto volumeAddress{reinterpret_cast<std::uintptr_t>(volumeMapping.data())};
            const auto sliceAddress{reinterpret_cast<std::uintptr_t>(sliceMapping.data())};
            if (sliceAddress < volumeAddress || sliceAddress - volumeAddress >= volumeMapping.size())
                return std::nullopt;

            const auto layouts{texture::GetBlockLinearMipLayout(
                volume.dimensions, volume.format->blockHeight, volume.format->blockWidth, volume.format->bpb,
                volume.format->blockHeight, volume.format->blockWidth, volume.format->bpb,
                volume.tileConfig.blockHeight, volume.tileConfig.blockDepth, volume.mipLevelCount)};
            const size_t offset{sliceAddress - volumeAddress};
            size_t levelOffset{};
            for (u32 level{}; level < layouts.size(); ++level) {
                const auto &layout{layouts[level]};
                if (slice.dimensions.width == layout.dimensions.width &&
                    slice.dimensions.height == layout.dimensions.height &&
                    slice.tileConfig.blockHeight == layout.blockHeight &&
                    slice.tileConfig.blockDepth == layout.blockDepth) {
                    const size_t mobs{util::DivideCeil<size_t>(layout.dimensions.depth, layout.blockDepth)};
                    const size_t mobSize{layout.blockLinearSize / mobs};
                    if (sliceMapping.size() == mobSize) {
                        const size_t gobSliceSize{512 * layout.blockHeight};
                        for (u32 z{}; z < layout.dimensions.depth; ++z) {
                            const size_t sliceOffset{levelOffset + (z / layout.blockDepth) * mobSize +
                                                     (z % layout.blockDepth) * gobSliceSize};
                            if (offset == sliceOffset)
                                return SliceLocation{level, z};
                        }
                    }
                }
                levelOffset += layout.blockLinearSize;
            }
            return std::nullopt;
        }
    }

    TextureManager::TextureManager(GPU &gpu) : gpu(gpu) {}

    std::shared_ptr<TextureView> TextureManager::FindOrCreate(const GuestTexture &guestTexture, ContextTag tag) {
        TRACE_EVENT("gpu", "TextureManager::FindOrCreate");

        auto guestMapping{guestTexture.mappings.front()};

        /*
         * Iterate over all textures that overlap with the first mapping of the guest texture and compare the mappings:
         * 1) All mappings match up perfectly, we check that the rest of the supplied mappings correspond to mappings in the texture
         * 1.1) If they match as well, we check for format/dimensions/tiling config matching the texture and return or move onto (3)
         * 2) Only a contiguous range of mappings match, we check for if the overlap is meaningful with layout math, it can go two ways:
         * 2.1) If there is a meaningful overlap, we check for format/dimensions/tiling config compatibility and return or move onto (3)
         * 2.2) If there isn't, we move onto (3)
         * 3) If there's another overlap we go back to (1) with it else we go to (4)
         * 4) We check all the overlapping texture for if they're in the texture pool:
         * 4.1) If they are, we do nothing to them
         * 4.2) If they aren't, we delete them from the map
         * 5) Create a new texture and insert it in the map then return it
         */

        std::shared_ptr<Texture> match{};
        boost::container::small_vector<std::shared_ptr<Texture>, 4> matches{};
        auto mappingEnd{std::upper_bound(textures.begin(), textures.end(), guestMapping, [guestMapping](const auto &value, const auto &element) {
            return guestMapping.end() < element.end();
        })}, hostMapping{std::lower_bound(mappingEnd, textures.end(), guestMapping, [guestMapping](const auto &value, const auto &element) {
            return guestMapping.begin() < element.end();
        })};

        std::shared_ptr<Texture> fullMatch{};
        std::shared_ptr<Texture> layerMipMatch{};
        std::shared_ptr<Texture> sliceMatch{};
        boost::container::small_vector<std::shared_ptr<Texture>, 8> sliceOverlaps{};
        u32 matchLevel{};
        u32 matchLayer{};
        SliceLocation sliceLocation{};
        bool had3DOverlap{};

        while (hostMapping != textures.begin() && (--hostMapping)->end() > guestMapping.begin()) {
            auto &hostMappings{hostMapping->texture->guest->mappings};
            const auto &hostGuest{*hostMapping->texture->guest};
            const bool hostIs3D{hostGuest.GetImageType() == vk::ImageType::e3D};
            const bool requestIs3D{guestTexture.GetImageType() == vk::ImageType::e3D};
            const bool log3DPair{(hostIs3D || requestIs3D) &&
                                 hostMapping->begin() < guestMapping.end() && guestMapping.begin() < hostMapping->end()};
            if (log3DPair) {
                had3DOverlap = true;
                LOGI("TEXMAN-3D-SLICE: host3D={}, request3D={}, hostView={}, requestView={}, hostSize=0x{:X}, requestSize=0x{:X}, hostWidth={}, hostHeight={}, requestWidth={}, requestHeight={}, hostDepth={}, requestDepth={}, hostMipLevels={}, requestMipLevels={}, hostLayers={}, requestBaseLayer={}, requestLayers={}, hostBlockDepth={}, requestBlockDepth={}, hostStart={}, requestStart={}, contained={}",
                     hostIs3D, requestIs3D, static_cast<u32>(hostGuest.viewType), static_cast<u32>(guestTexture.viewType),
                     hostMapping->size(), guestMapping.size(), hostGuest.dimensions.width, hostGuest.dimensions.height,
                     guestTexture.dimensions.width, guestTexture.dimensions.height,
                     hostGuest.dimensions.depth, guestTexture.dimensions.depth,
                     hostGuest.mipLevelCount, guestTexture.mipLevelCount, hostGuest.layerCount,
                     guestTexture.baseArrayLayer, guestTexture.GetViewLayerCount(),
                     hostGuest.tileConfig.mode == texture::TileMode::Block ? hostGuest.tileConfig.blockDepth : 0,
                     guestTexture.tileConfig.mode == texture::TileMode::Block ? guestTexture.tileConfig.blockDepth : 0,
                     fmt::ptr(hostMapping->data()), fmt::ptr(guestMapping.data()),
                     hostMapping->contains(guestMapping));
            }
            if (hostMapping->texture->replaced)
                continue;

            if (hostIs3D != requestIs3D) {
                if (hostIs3D) {
                    if (auto location{Find3DSlice(hostGuest, *hostMapping, guestTexture, guestMapping)}) {
                        sliceMatch = hostMapping->texture;
                        sliceLocation = *location;
                        if (log3DPair)
                            LOGI("TEXMAN-3D-SLICE: reused 3D slice, mip={}, slice={}", location->level, location->slice);
                    } else if (log3DPair && std::find(matches.begin(), matches.end(), hostMapping->texture) == matches.end()) {
                        matches.push_back(hostMapping->texture);
                    }
                } else if (auto location{Find3DSlice(guestTexture, guestMapping, hostGuest, *hostMapping)}) {
                    if (std::find(sliceOverlaps.begin(), sliceOverlaps.end(), hostMapping->texture) == sliceOverlaps.end())
                        sliceOverlaps.push_back(hostMapping->texture);
                    if (log3DPair)
                        LOGI("TEXMAN-3D-SLICE: read back 2D slice before creating 3D image, mip={}, slice={}",
                             location->level, location->slice);
                } else if (log3DPair && std::find(matches.begin(), matches.end(), hostMapping->texture) == matches.end()) {
                    matches.push_back(hostMapping->texture);
                }
                continue;
            }
            if (!hostMapping->contains(guestMapping))
                continue;

            // We need to check that all corresponding mappings in the candidate texture and the guest texture match up
            // Only the start of the first matched mapping and the end of the last mapping can not match up as this is the case for views
            auto firstHostMapping{hostMapping->iterator};
            auto lastGuestMapping{guestTexture.mappings.back()};
            auto lastHostMapping{std::find_if(firstHostMapping, hostMappings.end(), [&lastGuestMapping](const span<u8> &it) {
                return lastGuestMapping.begin() > it.begin() && lastGuestMapping.end() > it.end();
            })}; //!< A past-the-end iterator for the last host mapping, the final valid mapping is prior to this iterator
            bool mappingMatch{std::equal(firstHostMapping, lastHostMapping, guestTexture.mappings.begin(), guestTexture.mappings.end(), [](const span<u8> &lhs, const span<u8> &rhs) {
                return lhs.end() == rhs.end(); // We check end() here to implicitly ignore any offset from the first mapping
            })};

            if (firstHostMapping == hostMappings.begin() && firstHostMapping->begin() == guestMapping.begin() && mappingMatch && lastHostMapping == hostMappings.end() && lastGuestMapping.end() == std::prev(lastHostMapping)->end()) {
                // We've gotten a perfect 1:1 match for *all* mappings from the start to end, we just need to check for compatibility aside from this
                auto &matchGuestTexture{*hostMapping->texture->guest};
                if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                    ((((matchGuestTexture.dimensions.width == guestTexture.dimensions.width &&
                        matchGuestTexture.dimensions.height == guestTexture.dimensions.height) || matchGuestTexture.CalculateLayerSize() == guestTexture.CalculateLayerSize()) &&
                        matchGuestTexture.GetViewDepth() <= guestTexture.GetViewDepth())
                        || matchGuestTexture.viewMipBase > 0)
                    && matchGuestTexture.tileConfig == guestTexture.tileConfig) {
                    if (log3DPair)
                        LOGI("TEXMAN-3D-SLICE: reused full mapping");
                    fullMatch = hostMapping->texture;
                } else {
                    if (log3DPair)
                        LOGI("TEXMAN-3D-SLICE: rejected full mapping: formatCompatible={}, tilingCompatible={}",
                             matchGuestTexture.format->IsCompatible(*guestTexture.format), matchGuestTexture.tileConfig == guestTexture.tileConfig);
                    matches.push_back(hostMapping->texture);
                }
            } else {
                auto &matchGuestTexture{*hostMapping->texture->guest};
                if (matchGuestTexture.format->IsCompatible(*guestTexture.format) && matchGuestTexture.tileConfig == guestTexture.tileConfig &&
                        (!layerMipMatch || (matchGuestTexture.GetViewLayerCount() >= layerMipMatch->guest->GetViewLayerCount() && matchGuestTexture.mipLevelCount >= layerMipMatch->guest->mipLevelCount))) {
                    size_t memOffset{static_cast<size_t>(guestMapping.data() - hostMapping->texture->guest->mappings.front().data())};
                    size_t layerMemOffset{};
                    bool matched{};
                    for (u32 layer{}; layer < hostMapping->texture->layerCount; layer++) {
                        u32 level{};
                        size_t levelMemOffset{};

                        for (auto &mipLevel : hostMapping->texture->mipLayouts) {
                            if (layerMemOffset + levelMemOffset == memOffset) {
                                if (mipLevel.blockLinearSize == guestTexture.CalculateLayerSize()) {
                                    matched = true;
                                    matchLayer = layer;
                                    matchLevel = level;
                                    break;
                                }
                                level++;
                                levelMemOffset += mipLevel.blockLinearSize;
                            }
                        }

                        if (matched)
                            break;
                        layerMemOffset += matchGuestTexture.GetLayerStride();
                    }

                    if (matched) {
                        if (log3DPair)
                            LOGI("TEXMAN-3D-SLICE: reused partial mapping, mip={}, layer={}", matchLevel, matchLayer);
                        if (layerMipMatch)
                            layerMipMatch->replaced = true;

                        if (fullMatch)
                            fullMatch->replaced = true;

                        layerMipMatch = hostMapping->texture;
                    }
                }
            }
         }

        if (!sliceOverlaps.empty() && fullMatch) {
            fullMatch->SynchronizeGuest(false, true);
            fullMatch->replaced = true;
            fullMatch = {};
        }
        if (!sliceOverlaps.empty() && layerMipMatch) {
            layerMipMatch->SynchronizeGuest(false, true);
            layerMipMatch->replaced = true;
            layerMipMatch = {};
        }

        if (sliceMatch) {
            ContextLock textureLock{tag, *sliceMatch};
            return sliceMatch->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
                .aspectMask = guestTexture.aspect,
                .baseMipLevel = sliceLocation.level,
                .levelCount = 1,
                .baseArrayLayer = sliceLocation.slice,
                .layerCount = 1,
            }, guestTexture.format, guestTexture.swizzle);
        } else if (layerMipMatch && sliceOverlaps.empty()) {
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

        for (auto &texture : matches)
            texture->SynchronizeGuest(false, true);

        for (auto &texture : sliceOverlaps) {
            if (std::find(matches.begin(), matches.end(), texture) == matches.end())
                texture->SynchronizeGuest(false, true);
            texture->replaced = true;
        }

        if (had3DOverlap)
            LOGI("TEXMAN-3D-SLICE: no compatible mapping, creating a separate texture");

        // Create a texture as we cannot find one that matches
        auto texture{std::make_shared<Texture>(gpu, guestTexture)};
        texture->SetupGuestMappings();
        texture->TransitionLayout(vk::ImageLayout::eGeneral);
        auto it{texture->guest->mappings.begin()};
        textures.emplace(mappingEnd, TextureMapping{texture, it, guestMapping});
        while ((++it) != texture->guest->mappings.end()) {
            guestMapping = *it;
            auto mapping{std::upper_bound(textures.begin(), textures.end(), guestMapping)};
            // TODO: Delete overlapping textures that aren't in texture pool
            textures.emplace(mapping, TextureMapping{texture, it, guestMapping});
        }

        return texture->GetView(guestTexture.viewType, vk::ImageSubresourceRange{
            .aspectMask = guestTexture.aspect,
            .baseMipLevel = guestTexture.viewMipBase,
            .levelCount = guestTexture.viewMipCount,
            .baseArrayLayer = guestTexture.baseArrayLayer,
            .layerCount = guestTexture.GetViewLayerCount(),
        }, guestTexture.format, guestTexture.swizzle);
    }
}
