// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <atomic>
#include <common/trace.h>
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
        u32 matchLevel{};
        u32 matchLayer{};

        size_t overlapCount{};
        size_t skippedOverlapCount{};
        size_t incompatibleFullCount{};
        size_t partialNoMatchCount{};

        auto takeTraceSlot{[] {
            static std::atomic<u32> traceCount{};
            return traceCount.fetch_add(1, std::memory_order_relaxed) < 2048;
        }};

        while (hostMapping != textures.begin() && (--hostMapping)->end() > guestMapping.begin()) {
            auto &hostMappings{hostMapping->texture->guest->mappings};
            overlapCount++;

            if (!hostMapping->contains(guestMapping) || hostMapping->texture->replaced) {
                skippedOverlapCount++;
                if (takeTraceSlot()) {
                    LOGI("TEXMAN-MATCH overlap-skip req={} size=0x{:X} candidate={} map={} mapSize=0x{:X} contains={} replaced={} rt={}",
                         static_cast<const void *>(guestMapping.data()), guestMapping.size(),
                         static_cast<const void *>(hostMapping->texture.get()),
                         static_cast<const void *>(hostMapping->data()), hostMapping->size(),
                         hostMapping->contains(guestMapping), hostMapping->texture->replaced,
                         hostMapping->texture->everUsedAsRt);
                }
                continue;
            }

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
                    fullMatch = hostMapping->texture;
                    if (takeTraceSlot()) {
                        LOGI("TEXMAN-MATCH full req={} host={} fmt={} dims={}x{}x{} mip={}/{} layer={}/{} rt={}",
                             static_cast<const void *>(guestMapping.data()),
                             static_cast<const void *>(fullMatch.get()),
                             static_cast<u32>(guestTexture.format->vkFormat),
                             guestTexture.dimensions.width, guestTexture.dimensions.height, guestTexture.dimensions.depth,
                             guestTexture.viewMipBase, guestTexture.viewMipCount,
                             guestTexture.baseArrayLayer, guestTexture.GetViewLayerCount(),
                             fullMatch->everUsedAsRt);
                    }
                } else {
                    incompatibleFullCount++;
                    matches.push_back(hostMapping->texture);

                    if (takeTraceSlot()) {
                        const bool formatCompatible{matchGuestTexture.format->IsCompatible(*guestTexture.format)};
                        const bool dimensionsCompatible{
                            matchGuestTexture.dimensions.width == guestTexture.dimensions.width &&
                            matchGuestTexture.dimensions.height == guestTexture.dimensions.height
                        };
                        const bool layerSizeCompatible{matchGuestTexture.CalculateLayerSize() == guestTexture.CalculateLayerSize()};
                        const bool depthCompatible{matchGuestTexture.GetViewDepth() <= guestTexture.GetViewDepth()};
                        const bool mipViewCompatible{matchGuestTexture.viewMipBase > 0};
                        const bool tileCompatible{matchGuestTexture.tileConfig == guestTexture.tileConfig};

                        LOGI("TEXMAN-MATCH full-incompatible req={} host={} fmt={} dims={} layerSize={} depth={} mipView={} tile={} old={}x{} new={}x{} oldFmt={} newFmt={} rt={}",
                             static_cast<const void *>(guestMapping.data()),
                             static_cast<const void *>(hostMapping->texture.get()),
                             formatCompatible, dimensionsCompatible, layerSizeCompatible, depthCompatible,
                             mipViewCompatible, tileCompatible,
                             matchGuestTexture.dimensions.width, matchGuestTexture.dimensions.height,
                             guestTexture.dimensions.width, guestTexture.dimensions.height,
                             static_cast<u32>(matchGuestTexture.format->vkFormat),
                             static_cast<u32>(guestTexture.format->vkFormat),
                             hostMapping->texture->everUsedAsRt);
                    }
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
                        if (layerMipMatch)
                            layerMipMatch->replaced = true;

                        if (fullMatch)
                            fullMatch->replaced = true;

                        layerMipMatch = hostMapping->texture;
                        if (takeTraceSlot()) {
                            LOGI("TEXMAN-MATCH layer-mip req={} host={} memOff=0x{:X} level={} layer={} fmt={} rt={}",
                                 static_cast<const void *>(guestMapping.data()),
                                 static_cast<const void *>(layerMipMatch.get()),
                                 memOffset, matchLevel, matchLayer,
                                 static_cast<u32>(guestTexture.format->vkFormat),
                                 layerMipMatch->everUsedAsRt);
                        }
                    } else {
                        partialNoMatchCount++;
                        if (takeTraceSlot()) {
                            LOGI("TEXMAN-MATCH partial-no-subresource req={} host={} memOff=0x{:X} reqLayerSize=0x{:X} oldLayerStride=0x{:X} oldLevels={} oldLayers={} rt={}",
                                 static_cast<const void *>(guestMapping.data()),
                                 static_cast<const void *>(hostMapping->texture.get()),
                                 memOffset, guestTexture.CalculateLayerSize(), matchGuestTexture.GetLayerStride(),
                                 hostMapping->texture->levelCount, hostMapping->texture->layerCount,
                                 hostMapping->texture->everUsedAsRt);
                        }
                    }
                } else if (takeTraceSlot()) {
                    const bool formatCompatible{matchGuestTexture.format->IsCompatible(*guestTexture.format)};
                    const bool tileCompatible{matchGuestTexture.tileConfig == guestTexture.tileConfig};
                    const bool candidatePreferred{
                        !layerMipMatch ||
                        (matchGuestTexture.GetViewLayerCount() >= layerMipMatch->guest->GetViewLayerCount() &&
                         matchGuestTexture.mipLevelCount >= layerMipMatch->guest->mipLevelCount)
                    };

                    LOGI("TEXMAN-MATCH partial-incompatible req={} host={} fmt={} tile={} preferred={} oldFmt={} newFmt={} rt={}",
                         static_cast<const void *>(guestMapping.data()),
                         static_cast<const void *>(hostMapping->texture.get()),
                         formatCompatible, tileCompatible, candidatePreferred,
                         static_cast<u32>(matchGuestTexture.format->vkFormat),
                         static_cast<u32>(guestTexture.format->vkFormat),
                         hostMapping->texture->everUsedAsRt);
                }
            }
         }

        if (layerMipMatch) {
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

        if (overlapCount && takeTraceSlot()) {
            LOGI("TEXMAN-MATCH create-new-overlap req={} size=0x{:X} overlaps={} skipped={} fullBad={} partialNoMatch={} fmt={} dims={}x{}x{} mip={}/{} layer={}/{} tileMode={}",
                 static_cast<const void *>(guestMapping.data()), guestMapping.size(),
                 overlapCount, skippedOverlapCount, incompatibleFullCount, partialNoMatchCount,
                 static_cast<u32>(guestTexture.format->vkFormat),
                 guestTexture.dimensions.width, guestTexture.dimensions.height, guestTexture.dimensions.depth,
                 guestTexture.viewMipBase, guestTexture.viewMipCount,
                 guestTexture.baseArrayLayer, guestTexture.GetViewLayerCount(),
                 static_cast<u32>(guestTexture.tileConfig.mode));
        }

        // Create a texture as we cannot find one that matches
        auto texture{std::make_shared<Texture>(gpu, guestTexture)};
        if (overlapCount && takeTraceSlot())
            LOGI("TEXMAN-MATCH created host={} req={}", static_cast<const void *>(texture.get()), static_cast<const void *>(guestMapping.data()));
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
