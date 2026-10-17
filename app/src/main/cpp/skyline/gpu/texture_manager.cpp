// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include <gpu.h>
#include "texture/layout.h"
#include "texture_manager.h"
#include "texture/backing_promotion.h"
#include "texture/view_range.h"
#include "texture/guest_range.h"

namespace skyline::gpu {
    texture::Format ConvertHostCompatibleFormat(texture::Format format, const TraitManager &traits);
    TextureManager::TextureManager(GPU &gpu) : gpu(gpu) {}

    std::shared_ptr<TextureView> TextureManager::FindOrCreate(const GuestTexture &guestTexture, ContextTag tag) {
        TRACE_EVENT("gpu", "TextureManager::FindOrCreate");

        // Never dereference an empty guest mapping sequence or try to resolve
        // invalid physical spans through the Texman overlap table.
        if (guestTexture.mappings.empty() || guestTexture.mappings.front().empty() || !guestTexture.MappingsValid())
            throw exception("Cannot resolve texture with missing or invalid guest mappings");
        const texture::GuestResourceRanges guestRanges{guestTexture.mappings};
        if (!guestRanges.Valid())
            throw exception("Invalid guest texture mapping ranges");
        if (!texture::SubresourceRangeFits(guestTexture.mipLevelCount, guestTexture.viewMipBase, guestTexture.viewMipCount)) {
            throw exception("Guest texture view exceeds its declared mip chain");
        }
        const vk::ImageSubresourceRange requestedRange{
            guestTexture.aspect, guestTexture.viewMipBase,
            guestTexture.viewMipCount == VK_REMAINING_MIP_LEVELS && guestTexture.viewMipBase < guestTexture.mipLevelCount ?
                guestTexture.mipLevelCount - guestTexture.viewMipBase : guestTexture.viewMipCount,
            guestTexture.baseArrayLayer, guestTexture.GetViewLayerCount(),
        };
        const auto requestedStride{guestTexture.layerStride ? guestTexture.layerStride : guestTexture.CalculateLayerSize()};

        // The V2 index finds every physical overlap, including nested mappings
        // and resources that overlap only a later query segment.
        boost::container::small_vector<std::shared_ptr<Texture>, 4> matches{};
        const auto mappingLookup{mappingCache.Lookup(guestRanges)};
        boost::container::small_vector<const texture::TextureStorage *, 4> visited;

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

        for (const auto *mapping : mappingLookup.firstMappingOverlaps) {
            const auto &hostMapping{mapping->storage};
            if (!hostMapping || !hostMapping->texture || hostMapping->texture->replaced ||
                std::find(visited.begin(), visited.end(), hostMapping.get()) != visited.end())
                continue;
            visited.push_back(hostMapping.get());
            const auto &hostRanges{hostMapping->ranges};
            const auto mappedOffset{hostRanges.FindContainedOffset(guestRanges)};
            if (!mappedOffset)
                continue;

            if (*mappedOffset == 0 && hostRanges.Size() == guestRanges.Size()) {
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
                    if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                        matchGuestTexture.tileConfig == guestTexture.tileConfig &&
                        ((matchGuestTexture.dimensions.width == guestTexture.dimensions.width &&
                          matchGuestTexture.dimensions.height == guestTexture.dimensions.height &&
                          matchGuestTexture.GetViewDepth() <= guestTexture.GetViewDepth()) || matchGuestTexture.viewMipBase > 0)) {
                        // The old full-match predicate would have reused this
                        // image, despite incompatible backing bounds/layout. Do
                        // not send it through unsynchronized overlap readback:
                        // that would create a second, incoherent GPU writer.
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
                    const size_t memOffset{*mappedOffset};

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
                    const size_t memOffset{*mappedOffset};

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
        if (unrepresentableMatch || separateGroup) {
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

        // No reusable view was selected. Preserve the guest-memory fallback for
        // every overlap, even if the first query mapping does not touch it.
        for (const auto &storage : mappingLookup.storages)
            if (storage && storage->texture && !storage->texture->replaced &&
                std::find(matches.begin(), matches.end(), storage->texture) == matches.end())
                matches.push_back(storage->texture);

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

            for (const auto &storage : mappingCache.StoragesInMappingOrder()) {
                auto source{storage->texture};
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

                // Require the whole source to fit, including every later mapping.
                auto sourceOffset{guestRanges.FindContainedOffset(storage->ranges)};
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

        if (tag) {
            TextureAliasReconciliationRequest request{};
            for (const auto &texture : matches) {
                if ((!separateGroup || texture->aliasGroup.load() != separateGroup) &&
                    texture->tag.load() == tag && texture->gpuWriteSerial != texture->gpuReadbackSerial) {
                    // RecordGpuWrite precedes command-node submission. Neither
                    // dirtyState nor cycle necessarily describes that write yet.
                    // Restart preparation at the existing executor boundary.
                    request.guestSynchronization.push_back(texture);
                    std::scoped_lock stateLock{texture->stateMutex};
                    if (!texture->memoryFreed)
                        request.guestMemoryLeases.push_back(texture->PinGuestMemory());
                }
            }
            if (!request.guestSynchronization.empty())
                throw request;
        }

        for (auto &texture : matches) {
            if (!separateGroup || texture->aliasGroup.load() != separateGroup) {
                ContextLock lock{tag, *texture};
                texture->SynchronizeGuest(false, true);
            }
        }

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
        mappingCache.Insert(std::make_shared<texture::TextureStorage>(
            texture, texture::GuestResourceRanges{texture->guest->mappings}));

        if (!tag) texture->ReconcileAliases();
        return texture->GetView(guestTexture.viewType, requestedRange, guestTexture.format, guestTexture.swizzle, tag);
    }
}
