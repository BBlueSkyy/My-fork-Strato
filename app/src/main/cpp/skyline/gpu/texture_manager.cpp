// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include "texture/layout.h"
#include "texture_manager.h"

namespace skyline::gpu {
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
        std::shared_ptr<Texture> depthSliceMatch{};
        u32 matchLevel{};
        u32 matchLayer{};
        u32 depthSliceLevel{};
        u32 depthSlice{};
        u32 depthSliceParentDepth{};
        bool saw3DOverlap{};

        while (hostMapping != textures.begin() && (--hostMapping)->end() > guestMapping.begin()) {
            auto &hostMappings{hostMapping->texture->guest->mappings};
            if (!hostMapping->contains(guestMapping) || hostMapping->texture->replaced)
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

                if (matchGuestTexture.GetImageType() != guestTexture.GetImageType() &&
                    (matchGuestTexture.GetImageType() == vk::ImageType::e3D ||
                     guestTexture.GetImageType() == vk::ImageType::e3D))
                    LOGI("[TEX3D] reject-cross-image-full-match hostImageType={} reqImageType={} "
                         "host={}x{}x{} req={}x{}x{}",
                         static_cast<u32>(matchGuestTexture.GetImageType()),
                         static_cast<u32>(guestTexture.GetImageType()),
                         matchGuestTexture.dimensions.width, matchGuestTexture.dimensions.height, matchGuestTexture.dimensions.depth,
                         guestTexture.dimensions.width, guestTexture.dimensions.height, guestTexture.dimensions.depth);

                if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                    matchGuestTexture.GetImageType() == guestTexture.GetImageType() &&
                    ((((matchGuestTexture.dimensions.width == guestTexture.dimensions.width &&
                        matchGuestTexture.dimensions.height == guestTexture.dimensions.height) || matchGuestTexture.CalculateLayerSize() == guestTexture.CalculateLayerSize()) &&
                        matchGuestTexture.GetViewDepth() <= guestTexture.GetViewDepth())
                        || matchGuestTexture.viewMipBase > 0)
                    && matchGuestTexture.tileConfig == guestTexture.tileConfig) {
                    fullMatch = hostMapping->texture;
                } else {
                    matches.push_back(hostMapping->texture);
                }
            } else {
                auto &matchGuestTexture{*hostMapping->texture->guest};

                const bool is3DTo2DOverlap{
                    matchGuestTexture.GetImageType() == vk::ImageType::e3D &&
                    (guestTexture.viewType == vk::ImageViewType::e2D ||
                     guestTexture.viewType == vk::ImageViewType::e2DArray)
                };
                if (is3DTo2DOverlap) {
                    saw3DOverlap = true;
                    size_t diagnosticOffset{};
                    for (auto it{hostMappings.begin()}; it != hostMapping->iterator; ++it)
                        diagnosticOffset += it->size();
                    diagnosticOffset += static_cast<size_t>(guestMapping.data() - hostMapping->iterator->data());

                    LOGI("[TEX3D] overlap req={}x{}x{} view={} layers={} mips={}/{}+{} block={}x{} "
                         "host={}x{}x{} view={} layers={} mips={} block={}x{} offset=0x{:X}",
                         guestTexture.dimensions.width, guestTexture.dimensions.height, guestTexture.dimensions.depth,
                         static_cast<u32>(guestTexture.viewType), guestTexture.layerCount,
                         guestTexture.mipLevelCount, guestTexture.viewMipBase, guestTexture.viewMipCount,
                         guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth,
                         matchGuestTexture.dimensions.width, matchGuestTexture.dimensions.height, matchGuestTexture.dimensions.depth,
                         static_cast<u32>(matchGuestTexture.viewType), matchGuestTexture.layerCount,
                         matchGuestTexture.mipLevelCount,
                         matchGuestTexture.tileConfig.blockHeight, matchGuestTexture.tileConfig.blockDepth,
                         diagnosticOffset);
                }

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
                    matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
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
                                        LOGI("[TEX3D] matched mip={} slice={} parentDepth={} offset=0x{:X} "
                                             "reqLayers={} block={}x{}",
                                             level, slice, mipLevel.dimensions.depth, memOffset,
                                             viewLayerCount, mipLevel.blockHeight, mipLevel.blockDepth);
                                    }
                                    break;
                                }
                            }
                        }

                        levelMemOffset += mipLevel.blockLinearSize;
                        ++level;
                    }

                    if (!depthSliceMatch)
                        LOGI("[TEX3D] no-slice-match offset=0x{:X} req={}x{}x{} reqBlock={}x{} hostLevels={}",
                             memOffset,
                             guestTexture.dimensions.width, guestTexture.dimensions.height, guestTexture.dimensions.depth,
                             guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth,
                             hostMapping->texture->mipLayouts.size());
                }

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
                        if (layerMipMatch)
                            layerMipMatch->replaced = true;

                        if (fullMatch)
                            fullMatch->replaced = true;

                        layerMipMatch = hostMapping->texture;
                    }
                }
            }
         }

        if (depthSliceMatch) {
            LOGI("[TEX3D] using-3d-backing mip={} slice={} layers={}",
                 depthSliceLevel, depthSlice, guestTexture.GetViewLayerCount());

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

        for (auto &texture : matches)
            texture->SynchronizeGuest(false, true);

        // Create a texture as we cannot find one that matches
        if (saw3DOverlap)
            LOGI("[TEX3D] creating-separate-storage req={}x{}x{} view={} layers={} mips={}/{}+{} block={}x{} size=0x{:X}",
                 guestTexture.dimensions.width, guestTexture.dimensions.height, guestTexture.dimensions.depth,
                 static_cast<u32>(guestTexture.viewType), guestTexture.layerCount,
                 guestTexture.mipLevelCount, guestTexture.viewMipBase, guestTexture.viewMipCount,
                 guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth,
                 guestTexture.CalculateLayerSize());

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
