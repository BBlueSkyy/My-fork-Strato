// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include <gpu.h>
#include "texture/layout.h"
#include "texture_manager.h"
#include "texture/backing_promotion.h"
#include "texture/view_range.h"

namespace skyline::gpu {
    texture::Format ConvertHostCompatibleFormat(texture::Format format, const TraitManager &traits);
    TextureManager::TextureManager(GPU &gpu) : gpu(gpu) {}

    std::shared_ptr<texture::AliasGroup> TextureManager::PrepareMappingAliases(const GuestTexture &guest, ContextTag tag) {
        if (sparseTextures.empty() && !guest.HasSparseMappings()) return {};
        auto overlaps{[&](const std::shared_ptr<Texture> &texture) {
            return texture && !texture->replaced && !texture->aliasRetired.load() && texture->guest &&
                Texture::GuestMappingsOverlap(guest, *texture->guest);
        }};
        std::vector<std::shared_ptr<Texture>> members;
        for (const auto &texture : sparseTextures) if (overlaps(texture)) members.push_back(texture);
        if (members.empty() && !guest.HasSparseMappings()) return {};
        for (const auto &mapping : textures)
            if (overlaps(mapping.texture) && std::find(members.begin(), members.end(), mapping.texture) == members.end()) members.push_back(mapping.texture);
        if (members.empty()) {
            auto group{std::make_shared<texture::AliasGroup>()}; group->mappingAliases = true; return group;
        }
        const auto group{members.front()->aliasGroup.load()};
        if (group && group->mappingAliases && std::all_of(members.begin(), members.end(), [&](const auto &member) { return member->aliasGroup.load() == group; })) return group;
        if (tag) {
            TextureAliasReconciliationRequest request{{}, members};
            request.mappingRegistration = true;
            for (const auto &member : members) {
                std::scoped_lock state{member->stateMutex};
                if (!member->memoryFreed) request.guestMemoryLeases.push_back(member->PinGuestMemory());
            }
            throw request;
        }
        return Texture::RegisterMappingAliases(std::move(members));
    }

    std::shared_ptr<TextureView> TextureManager::FindOrCreateSparse(const GuestTexture &guest, ContextTag tag) {
        const vk::ImageSubresourceRange range{guest.aspect, guest.viewMipBase,
            guest.viewMipCount == VK_REMAINING_MIP_LEVELS ? guest.mipLevelCount - guest.viewMipBase : guest.viewMipCount,
            guest.baseArrayLayer, guest.GetViewLayerCount()};
        for (const auto &texture : sparseTextures) {
            const auto &cached{*texture->guest};
            if (texture->replaced || cached.mappings != guest.mappings || cached.sparseMappings != guest.sparseMappings ||
                cached.mappingOffset != guest.mappingOffset || cached.dimensions != guest.dimensions || cached.format != guest.format ||
                !(cached.tileConfig == guest.tileConfig) || cached.layerCount != guest.layerCount || cached.mipLevelCount != guest.mipLevelCount ||
                cached.GetImageType() != guest.GetImageType() || (cached.layerStride ? cached.layerStride : cached.CalculateLayerSize()) != (guest.layerStride ? guest.layerStride : guest.CalculateLayerSize())) continue;
            if (!tag) texture->ReconcileAliases();
            ContextLock lock{tag, *texture};
            return texture->GetView(guest.viewType, range, guest.format, guest.swizzle, tag);
        }
        if (!Texture::CanMaterializeGuestLayout(guest)) throw exception("Cannot materialize sparse texture layout");
        auto group{PrepareMappingAliases(guest, tag)};
        auto texture{std::make_shared<Texture>(gpu, guest)};
        texture->SetupGuestMappings();
        texture->aliasReadOnly = true;
        {
            std::scoped_lock metadata{group->mutex};
            group->members.emplace_back(texture);
            texture->aliasGroup.store(group);
            texture->aliasStale = group->guestGeneration != group->generation;
        }
        texture->TransitionLayout(vk::ImageLayout::eGeneral);
        sparseTextures.push_back(texture);
        if (!tag) texture->ReconcileAliases();
        ContextLock lock{tag, *texture};
        return texture->GetView(guest.viewType, range, guest.format, guest.swizzle, tag);
    }

    std::shared_ptr<TextureView> TextureManager::FindOrCreate(const GuestTexture &guestTexture, ContextTag tag) {
        TRACE_EVENT("gpu", "TextureManager::FindOrCreate");

        // Never dereference an empty guest mapping sequence or try to resolve
        // invalid physical spans through the Texman overlap table.
        if (guestTexture.mappings.empty() || guestTexture.mappings.front().empty() || !guestTexture.MappingsValid())
            throw exception("Cannot resolve texture with missing or invalid guest mappings");
        auto guestMapping{guestTexture.mappings.front()};
        if (!texture::SubresourceRangeFits(guestTexture.mipLevelCount, guestTexture.viewMipBase, guestTexture.viewMipCount)) {
            throw exception("Guest texture view exceeds its declared mip chain");
        }
        if (guestTexture.HasSparseMappings()) return FindOrCreateSparse(guestTexture, tag);
        const vk::ImageSubresourceRange requestedRange{
            guestTexture.aspect, guestTexture.viewMipBase,
            guestTexture.viewMipCount == VK_REMAINING_MIP_LEVELS && guestTexture.viewMipBase < guestTexture.mipLevelCount ?
                guestTexture.mipLevelCount - guestTexture.viewMipBase : guestTexture.viewMipCount,
            guestTexture.baseArrayLayer, guestTexture.GetViewLayerCount(),
        };
        const auto requestedStride{guestTexture.layerStride ? guestTexture.layerStride : guestTexture.CalculateLayerSize()};

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

        boost::container::small_vector<std::shared_ptr<Texture>, 4> matches{};
        auto mappingEnd{std::upper_bound(textures.begin(), textures.end(), guestMapping, [guestMapping](const auto &value, const auto &element) {
            return guestMapping.end() < element.end();
        })}, hostMapping{std::lower_bound(mappingEnd, textures.end(), guestMapping, [guestMapping](const auto &value, const auto &element) {
            return guestMapping.begin() < element.end();
        })};

        std::vector<std::shared_ptr<Texture>> exactAliases;
        std::shared_ptr<texture::AliasGroup> separateGroup;
        std::shared_ptr<Texture> fullMatch{};
        std::shared_ptr<Texture> unrepresentableMatch{};
        std::shared_ptr<Texture> layerMipMatch{};
        std::shared_ptr<Texture> depthSliceMatch{};
        u32 matchLevel{};
        u32 matchLayer{};
        u32 depthSliceLevel{};
        u32 depthSlice{};
        u32 depthSliceParentDepth{};

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
                if (std::find(exactAliases.begin(), exactAliases.end(), hostMapping->texture) == exactAliases.end())
                    exactAliases.push_back(hostMapping->texture);
                auto &matchGuestTexture{*hostMapping->texture->guest};

                if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                    (matchGuestTexture.dimensions.width == guestTexture.dimensions.width &&
                       matchGuestTexture.dimensions.height == guestTexture.dimensions.height &&
                       matchGuestTexture.dimensions.depth == guestTexture.dimensions.depth)
                    && matchGuestTexture.GetImageType() == guestTexture.GetImageType()
                    && matchGuestTexture.tileConfig == guestTexture.tileConfig
                    && (guestTexture.GetViewLayerCount() == 1 || matchGuestTexture.GetLayerStride() == requestedStride)
                    && (hostMapping->texture->ViewRangeFits(guestTexture.viewType, requestedRange) ||
                        (hostMapping->texture->ViewRangeFits(guestTexture.viewType, requestedRange, guestTexture.mipLevelCount) &&
                         hostMapping->texture->CanGrowMipLevels(guestTexture.mipLevelCount)))) {
                    // Equal block-linear allocation sizes do not make differently-sized
                    // images interchangeable. A VkImageView cannot restrict a 16x1
                    // backing to the logical 1x1 extent described by a TIC, so reusing
                    // such a backing changes normalized sampling coordinates. Mip/layer
                    // aliases are handled by the representable view paths below.
                    fullMatch = hostMapping->texture;
                } else {
                    // Sampled TICs may auto-size Y while the render target keeps
                    // its declared block height. These are distinct encodings
                    // of the same allocation and must share writer history.
                    const bool sampledYLayoutAlias{
                        matchGuestTexture.tileConfig.mode == texture::TileMode::Block && guestTexture.tileConfig.mode == texture::TileMode::Block &&
                        matchGuestTexture.tileConfig.blockDepth == 1 && guestTexture.tileConfig.blockDepth == 1 &&
                        matchGuestTexture.tileConfig.blockHeight != guestTexture.tileConfig.blockHeight &&
                        matchGuestTexture.dimensions == guestTexture.dimensions &&
                        matchGuestTexture.dimensions.depth == 1 &&
                        matchGuestTexture.format->vkAspect == vk::ImageAspectFlagBits::eColor &&
                        !matchGuestTexture.baseArrayLayer && !guestTexture.baseArrayLayer &&
                        matchGuestTexture.GetImageType() == vk::ImageType::e2D && guestTexture.GetImageType() == vk::ImageType::e2D &&
                        matchGuestTexture.mipLevelCount == 1 && guestTexture.mipLevelCount == 1 &&
                        matchGuestTexture.layerCount == guestTexture.layerCount && matchGuestTexture.GetLayerStride() == requestedStride
                    };
                    if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                        (sampledYLayoutAlias || (matchGuestTexture.tileConfig == guestTexture.tileConfig &&
                         ((matchGuestTexture.dimensions.width == guestTexture.dimensions.width &&
                          matchGuestTexture.dimensions.height == guestTexture.dimensions.height &&
                          matchGuestTexture.GetViewDepth() <= guestTexture.GetViewDepth()) || matchGuestTexture.viewMipBase > 0)))) {
                        // These layouts cannot share a VkImage backing. Preserve
                        // their writer history through submission-safe alias
                        // reconciliation rather than unsynchronized readback.
                        unrepresentableMatch = hostMapping->texture;
                    } else {
                        matches.push_back(hostMapping->texture);
                    }
                }
            } else {
                auto &matchGuestTexture{*hostMapping->texture->guest};

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
                                    }
                                    break;
                                }
                            }
                        }

                        levelMemOffset += mipLevel.blockLinearSize;
                        ++level;
                    }

                }

                const bool sameTileConfig{matchGuestTexture.tileConfig == guestTexture.tileConfig};
                const bool blockMipCandidate{
                    matchGuestTexture.GetImageType() == vk::ImageType::e2D &&
                    guestTexture.GetImageType() == vk::ImageType::e2D &&
                    matchGuestTexture.tileConfig.mode == texture::TileMode::Block &&
                    guestTexture.tileConfig.mode == texture::TileMode::Block &&
                    guestTexture.mipLevelCount == 1 &&
                    guestTexture.viewMipBase == 0 &&
                    guestTexture.viewMipCount == 1
                };
                if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                        (sameTileConfig || blockMipCandidate) &&
                        (!layerMipMatch || (matchGuestTexture.GetViewLayerCount() >= layerMipMatch->guest->GetViewLayerCount() && matchGuestTexture.mipLevelCount >= layerMipMatch->guest->mipLevelCount))) {
                    size_t memOffset{static_cast<size_t>(guestMapping.data() - hostMapping->texture->guest->mappings.front().data())};

                    std::optional<texture::MipLevelLayout> requestedMipLayout;
                    if (blockMipCandidate) {
                        auto requestedLayouts{texture::GetBlockLinearMipLayout(
                            guestTexture.dimensions,
                            guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                            guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                            guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth, 1)};
                        if (!requestedLayouts.empty())
                            requestedMipLayout = requestedLayouts.front();
                    }

                    size_t layerMemOffset{};
                    bool matched{};
                    for (u32 layer{}; layer < hostMapping->texture->layerCount; layer++) {
                        u32 level{};
                        size_t levelMemOffset{};

                        for (const auto &mipLevel : hostMapping->texture->mipLayouts) {
                            bool layoutMatches{mipLevel.blockLinearSize == guestTexture.CalculateLayerSize()};
                            if (blockMipCandidate) {
                                layoutMatches = requestedMipLayout &&
                                    mipLevel.dimensions == guestTexture.dimensions &&
                                    mipLevel.blockHeight == requestedMipLayout->blockHeight &&
                                    mipLevel.blockDepth == requestedMipLayout->blockDepth &&
                                    mipLevel.blockLinearSize == requestedMipLayout->blockLinearSize;
                            }

                            if (layerMemOffset + levelMemOffset == memOffset && layoutMatches) {
                                auto range{requestedRange};
                                // Test before addition, including the whole requested
                                // range, not just the starting subresource.
                                if (range.baseMipLevel <= hostMapping->texture->levelCount - level &&
                                    range.baseArrayLayer <= hostMapping->texture->layerCount - layer) {
                                    range.baseMipLevel += level;
                                    range.baseArrayLayer += layer;
                                    if (hostMapping->texture->ViewRangeFits(guestTexture.viewType, range) &&
                                        (range.layerCount == 1 || matchGuestTexture.GetLayerStride() == requestedStride)) {
                                        matched = true;
                                        matchLayer = layer;
                                        matchLevel = level;
                                        break;
                                    }
                                }
                            }

                            // These offsets describe the parent's physical mip chain.
                            // Advancing only after an address equality (the old code)
                            // made every non-zero mip mathematically unreachable.
                            ++level;
                            levelMemOffset += mipLevel.blockLinearSize;
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

        bool requestedReadOnlyAlias{};
        for (const auto &alias : exactAliases)
            if (auto group{alias->aliasGroup.load()}) { separateGroup = std::move(group); break; }
        if ((unrepresentableMatch || separateGroup) && !(separateGroup && separateGroup->mappingAliases)) {
            const bool requestedMaterializable{Texture::CanMaterializeGuestLayout(guestTexture)};
            const bool requestedLossless{Texture::IsGuestLayoutReversible(guestTexture) &&
                ConvertHostCompatibleFormat(guestTexture.format, gpu.traits) == guestTexture.format};
            if (!requestedMaterializable)
                throw exception("Cannot safely materialize this guest mip/layer layout on the overlapping texture backing");

            if (requestedLossless) {
                // Exact guest mappings identify history, not necessarily reversible
                // representations. Put an eligible layout first for registration;
                // the remaining history must pass its separate ownership check there.
                const auto eligibleEnd{std::stable_partition(exactAliases.begin(), exactAliases.end(), [&](const auto &alias) {
                    return alias->CanReconcileGuestLayout(guestTexture);
                })};
                bool registered{eligibleEnd == exactAliases.end() && separateGroup && std::all_of(exactAliases.begin(), exactAliases.end(), [&](const auto &alias) {
                    return alias->aliasGroup.load() == separateGroup;
                })};
                if (!registered) {
                    if (tag) {
                        std::vector<std::shared_ptr<void>> leases;
                        leases.reserve(exactAliases.size());
                        for (auto it{exactAliases.begin()}; it != eligibleEnd; ++it) leases.push_back((*it)->PinGuestMemory());
                        throw TextureAliasReconciliationRequest{{}, exactAliases, std::move(leases), std::make_shared<GuestTexture>(guestTexture)};
                    }
                    separateGroup = Texture::RegisterAliases(exactAliases, &guestTexture);
                }
            } else {
                // Host-converted or otherwise non-lossless representations may be
                // materialized for sampling, but they cannot own the guest bytes.
                // Establish/retain a group whose authority is a lossless existing
                // representation, then join the new representation read-only.
                requestedReadOnlyAlias = true;
                const auto writerEligibleEnd{std::stable_partition(exactAliases.begin(), exactAliases.end(), [](const auto &alias) {
                    return alias->guest && alias->CanReconcileGuestLayout(*alias->guest);
                })};
                if (writerEligibleEnd == exactAliases.begin())
                    throw exception("Cannot materialize a read-only texture alias without a lossless authoritative representation");

                bool registered{separateGroup && std::all_of(exactAliases.begin(), exactAliases.end(), [&](const auto &alias) {
                    return alias->aliasGroup.load() == separateGroup;
                })};
                if (!registered) {
                    if (tag) {
                        std::vector<std::shared_ptr<void>> leases;
                        leases.reserve(static_cast<size_t>(std::distance(exactAliases.begin(), writerEligibleEnd)));
                        for (auto it{exactAliases.begin()}; it != writerEligibleEnd; ++it) leases.push_back((*it)->PinGuestMemory());
                        throw TextureAliasReconciliationRequest{{}, exactAliases, std::move(leases), {}};
                    }
                    separateGroup = Texture::RegisterAliases(exactAliases);
                }
            }
        }

        if (fullMatch && fullMatch->aliasRetired.load()) fullMatch.reset();
        if (layerMipMatch && layerMipMatch->aliasRetired.load()) layerMipMatch.reset();
        if (depthSliceMatch && depthSliceMatch->aliasRetired.load()) depthSliceMatch.reset();

        if (!tag) {
            // BufferQueue lookups have no executor retry boundary. Reconcile
            // before taking the selected Texture's view lock.
            const auto selected{depthSliceMatch ? depthSliceMatch : layerMipMatch ? layerMipMatch : fullMatch};
            if (selected) selected->ReconcileAliases();
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
            }, guestTexture.format, guestTexture.swizzle, tag);
        } else if (layerMipMatch) {
            ContextLock textureLock{tag, *layerMipMatch};
            auto range{requestedRange};
            range.baseMipLevel += matchLevel;
            range.baseArrayLayer += matchLayer;
            return layerMipMatch->GetView(guestTexture.viewType, range, guestTexture.format, guestTexture.swizzle, tag);
        } else if (fullMatch) {
            ContextLock textureLock{tag, *fullMatch};
            if (!fullMatch->ViewRangeFits(guestTexture.viewType, requestedRange)) {
                // Submit only at a genuine backing incompatibility. Retrying also
                // rebuilds descriptors whose preparation used the previous image.
                if (tag)
                    throw TextureBackingPromotionRequest{fullMatch, fullMatch->format->vkFormat, guestTexture.mipLevelCount,
                        fullMatch->PinGuestMemoryForMipGrowth(guestTexture.mipLevelCount)};
                fullMatch->PromoteBackingForView(fullMatch->format->vkFormat, guestTexture.mipLevelCount);
            }
            return fullMatch->GetView(guestTexture.viewType, requestedRange, guestTexture.format, guestTexture.swizzle, tag);
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
                                } else if (!existing->texture->everUsedAsGpuWrite && source->everUsedAsGpuWrite) {
                                    // Storage-image writers are just as authoritative
                                    // as render targets for overlapping slice history.
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

        if (auto mappingGroup{PrepareMappingAliases(guestTexture, tag)}) {
            if (!Texture::CanMaterializeGuestLayout(guestTexture))
                throw exception("Cannot materialize a sparse mapping overlap");
            requestedReadOnlyAlias |= !Texture::IsGuestLayoutReversible(guestTexture) ||
                ConvertHostCompatibleFormat(guestTexture.format, gpu.traits) != guestTexture.format;
            separateGroup = std::move(mappingGroup);
        }
        for (auto &texture : matches)
            if (!separateGroup || texture->aliasGroup.load() != separateGroup)
                texture->SynchronizeGuest(false, true);

        // Create a texture as we cannot find one that matches


        auto texture{std::make_shared<Texture>(gpu, guestTexture)};
        texture->SetupGuestMappings();
        texture->aliasReadOnly = requestedReadOnlyAlias;
        if (separateGroup) {
            std::scoped_lock metadata{separateGroup->mutex};
            separateGroup->members.emplace_back(texture);
            texture->aliasGroup.store(separateGroup);
            texture->aliasStale = separateGroup->guestGeneration != separateGroup->generation;
        }
        texture->TransitionLayout(vk::ImageLayout::eGeneral);
        auto it{texture->guest->mappings.begin()};
        textures.emplace(mappingEnd, TextureMapping{texture, it, guestMapping});
        while ((++it) != texture->guest->mappings.end()) {
            guestMapping = *it;
            auto mapping{std::upper_bound(textures.begin(), textures.end(), guestMapping)};
            // TODO: Delete overlapping textures that aren't in texture pool
            textures.emplace(mapping, TextureMapping{texture, it, guestMapping});
        }

        if (!tag) texture->ReconcileAliases();
        return texture->GetView(guestTexture.viewType, requestedRange, guestTexture.format, guestTexture.swizzle, tag);
    }
}
