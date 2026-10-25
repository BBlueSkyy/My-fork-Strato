// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common/trace.h>
#include <gpu.h>
#include "texture/layout.h"
#include "texture_manager.h"
#include "texture/backing_promotion.h"
#include "texture/view_range.h"
#include "texture/secondary_overlap.h"
#include "texture/mapping_match.h"

namespace skyline::gpu {
    texture::Format ConvertHostCompatibleFormat(texture::Format format, const TraitManager &traits);
    TextureManager::TextureManager(GPU &gpu) : gpu(gpu) {}

    void TextureManager::SynchronizeGuestOverlaps(std::vector<std::shared_ptr<Texture>> sources, ContextTag tag) {
        if (sources.empty()) return;

        // A resource's cycle is assigned by SubmitInternal, not RecordGpuWrite.
        // Reading an attached producer now would wait on its *previous* cycle
        // and encode the old image while its new write is still an executor node.
        // Check every producer before touching guest bytes or taking more locks.
        for (const auto &source : sources) {
            // A historical serial mismatch is not a pending command: aliases
            // may have superseded that writer, or CPU bytes may now be current.
            // The matching tag means this thread owns the resource lock, so its
            // pending-write receipt can be inspected without another lock.
            if (tag && source->tag.load() == tag && source->pendingGpuWriteTag.load(std::memory_order_relaxed) == tag)
                throw TextureAliasReconciliationRequest{{}, {}, {}, {}, std::move(sources)};
        }

        auto resources{sources};
        std::sort(resources.begin(), resources.end(), [](const auto &a, const auto &b) {
            return std::less<Texture *>{}(a.get(), b.get());
        });
        resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
        std::vector<ContextLock<Texture>> locks;
        locks.reserve(resources.size());
        for (const auto &source : resources) locks.emplace_back(tag, *source);

        // Keep the original lookup order. Persistent partial-region authority is
        // separate work; sorting whole-image readbacks cannot implement it.
        for (const auto &source : sources) {
            // SynchronizeGuest requires the resource lock. Keep the state lock
            // across this bridge too, so traps cannot see its intermediate state.
            std::scoped_lock stateLock{source->stateMutex};
            const auto previousState{source->dirtyState};
            const bool previousMemoryFreed{source->memoryFreed};
            try {
                source->SynchronizeGuest(false, true);
            } catch (...) {
                source->dirtyState = previousState;
                source->memoryFreed = previousMemoryFreed;
                throw;
            }
        }
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

        overlaps.ForEachOverlap(guestMapping, textures, [&](const TextureMapping &mapping) {
            const auto *hostMapping{&mapping};
            if (hostMapping->texture->replaced || hostMapping->texture->aliasRetired.load())
                return;
            if (!hostMapping->contains(guestMapping)) {
                matches.push_back(hostMapping->texture);
                return;
            }
            auto &hostMappings{hostMapping->texture->guest->mappings};

            const auto mappingMatch{texture::MatchMappingSubrange(hostMappings, hostMapping->iterator, guestTexture.mappings)};
            if (!mappingMatch) {
                matches.push_back(hostMapping->texture);
                return;
            }

            if (mappingMatch->full) {
                // We've gotten a perfect 1:1 match for *all* mappings from the start to end, we just need to check for compatibility aside from this
                if (std::find(exactAliases.begin(), exactAliases.end(), hostMapping->texture) == exactAliases.end())
                    exactAliases.push_back(hostMapping->texture);
                auto &matchGuestTexture{*hostMapping->texture->guest};

                const bool sameTileLayout{[&] {
                    if (matchGuestTexture.tileConfig == guestTexture.tileConfig)
                        return true;
                    if (matchGuestTexture.tileConfig.mode != texture::TileMode::Block || guestTexture.tileConfig.mode != texture::TileMode::Block ||
                        matchGuestTexture.tileConfig.blockHeight != guestTexture.tileConfig.blockHeight ||
                        matchGuestTexture.tileConfig.blockDepth != guestTexture.tileConfig.blockDepth ||
                        matchGuestTexture.dimensions != guestTexture.dimensions || matchGuestTexture.mipLevelCount != guestTexture.mipLevelCount ||
                        !matchGuestTexture.format->IsCompatible(*guestTexture.format))
                        return false;
                    // Different TIC spacing can encode exactly the same bytes (e.g.
                    // an already-aligned RT). Keep sharing its authoritative backing.
                    auto dimensions{guestTexture.dimensions};
                    for (u32 level{}; level < guestTexture.mipLevelCount; ++level) {
                        auto width{[&](const GuestTexture &value) {
                            return texture::GetBlockLinearWidthGobs(dimensions,
                                value.format->blockWidth, value.format->blockHeight, value.format->bpb,
                                value.tileConfig.blockHeight, value.tileConfig.blockDepth, value.tileConfig.tileWidthSpacing);
                        }};
                        if (width(matchGuestTexture) != width(guestTexture)) return false;
                        dimensions.width = std::max(dimensions.width / 2, 1U);
                        dimensions.height = std::max(dimensions.height / 2, 1U);
                        dimensions.depth = std::max(dimensions.depth / 2, 1U);
                    }
                    return true;
                }()};
                if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                    (matchGuestTexture.dimensions.width == guestTexture.dimensions.width &&
                       matchGuestTexture.dimensions.height == guestTexture.dimensions.height &&
                       matchGuestTexture.dimensions.depth == guestTexture.dimensions.depth)
                    && matchGuestTexture.GetImageType() == guestTexture.GetImageType()
                    && sameTileLayout
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
                    // Sampled TICs may auto-size Y or pad X while the render target
                    // keeps its declared layout. These are distinct encodings
                    // of the same allocation and must share writer history.
                    const bool sampledLayoutAlias{
                        matchGuestTexture.tileConfig.mode == texture::TileMode::Block && guestTexture.tileConfig.mode == texture::TileMode::Block &&
                        matchGuestTexture.tileConfig.blockDepth == 1 && guestTexture.tileConfig.blockDepth == 1 &&
                        (matchGuestTexture.tileConfig.blockHeight != guestTexture.tileConfig.blockHeight ||
                         matchGuestTexture.tileConfig.tileWidthSpacing != guestTexture.tileConfig.tileWidthSpacing) &&
                        matchGuestTexture.dimensions == guestTexture.dimensions &&
                        matchGuestTexture.dimensions.depth == 1 &&
                        matchGuestTexture.format->vkAspect == vk::ImageAspectFlagBits::eColor &&
                        !matchGuestTexture.baseArrayLayer && !guestTexture.baseArrayLayer &&
                        matchGuestTexture.GetImageType() == vk::ImageType::e2D && guestTexture.GetImageType() == vk::ImageType::e2D &&
                        matchGuestTexture.mipLevelCount == 1 && guestTexture.mipLevelCount == 1 &&
                        matchGuestTexture.layerCount == guestTexture.layerCount && matchGuestTexture.GetLayerStride() == requestedStride
                    };
                    if (matchGuestTexture.format->IsCompatible(*guestTexture.format) &&
                        (sampledLayoutAlias || (sameTileLayout &&
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
                    const size_t memOffset{mappingMatch->offset};

                    size_t levelMemOffset{};
                    u32 level{};
                    for (const auto &mipLevel : hostMapping->texture->mipLayouts) {
                        if (mipLevel.dimensions.width == guestTexture.dimensions.width &&
                            mipLevel.dimensions.height == guestTexture.dimensions.height &&
                            mipLevel.blockHeight == guestTexture.tileConfig.blockHeight &&
                            mipLevel.blockDepth == guestTexture.tileConfig.blockDepth &&
                            texture::GetBlockLinearWidthGobs(mipLevel.dimensions,
                                matchGuestTexture.format->blockWidth, matchGuestTexture.format->blockHeight, matchGuestTexture.format->bpb,
                                mipLevel.blockHeight, mipLevel.blockDepth, mipLevel.tileWidthSpacing) ==
                            texture::GetBlockLinearWidthGobs(guestTexture.dimensions,
                                guestTexture.format->blockWidth, guestTexture.format->blockHeight, guestTexture.format->bpb,
                                guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth, guestTexture.tileConfig.tileWidthSpacing)) {
                            const u32 viewLayerCount{guestTexture.GetViewLayerCount()};
                            for (u32 slice{}; slice < mipLevel.dimensions.depth; ++slice) {
                                auto sliceOffset{texture::GetBlockLinearDepthSliceOffset(
                                    mipLevel.dimensions,
                                    matchGuestTexture.format->blockWidth,
                                    matchGuestTexture.format->blockHeight,
                                    matchGuestTexture.format->bpb,
                                    mipLevel.blockHeight,
                                    mipLevel.blockDepth,
                                    slice, mipLevel.tileWidthSpacing
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
                    const size_t memOffset{mappingMatch->offset};

                    std::optional<texture::MipLevelLayout> requestedMipLayout;
                    if (blockMipCandidate) {
                        auto requestedLayouts{texture::GetBlockLinearMipLayout(
                            guestTexture.dimensions,
                            guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                            guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                            guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth, 1,
                            guestTexture.tileConfig.tileWidthSpacing)};
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
        });

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

        if (guestTexture.GetImageType() == vk::ImageType::e3D &&
            guestTexture.tileConfig.mode == texture::TileMode::Block) {
            auto parentMipLayouts{texture::GetBlockLinearMipLayout(
                guestTexture.dimensions,
                guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                guestTexture.format->blockHeight, guestTexture.format->blockWidth, guestTexture.format->bpb,
                guestTexture.tileConfig.blockHeight, guestTexture.tileConfig.blockDepth,
                guestTexture.mipLevelCount, guestTexture.tileConfig.tileWidthSpacing
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
                        sourceGuest.tileConfig.blockDepth == mip.blockDepth &&
                        texture::GetBlockLinearWidthGobs(mip.dimensions,
                            guestTexture.format->blockWidth, guestTexture.format->blockHeight, guestTexture.format->bpb,
                            mip.blockHeight, mip.blockDepth, mip.tileWidthSpacing) ==
                        texture::GetBlockLinearWidthGobs(sourceGuest.dimensions,
                            sourceGuest.format->blockWidth, sourceGuest.format->blockHeight, sourceGuest.format->bpb,
                            sourceGuest.tileConfig.blockHeight, sourceGuest.tileConfig.blockDepth, sourceGuest.tileConfig.tileWidthSpacing)) {
                        for (u32 slice{}; slice < mip.dimensions.depth; ++slice) {
                            auto sliceOffset{texture::GetBlockLinearDepthSliceOffset(
                                mip.dimensions,
                                guestTexture.format->blockWidth,
                                guestTexture.format->blockHeight,
                                guestTexture.format->bpb,
                                mip.blockHeight,
                                mip.blockDepth,
                                slice, mip.tileWidthSpacing
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

        // The primary range matcher does not see textures overlapping only a
        // later physical mapping. Preserve their pending GPU writes before
        // materializing a new texture from the combined guest mappings.
        // Single-range lookups return immediately; no new hot-path scan.
        texture::ForEachSecondaryMappingOverlap(guestTexture.mappings, textures, overlaps,
            [&](const TextureMapping &entry) {
                if (!entry.texture || entry.texture->replaced || entry.texture->aliasRetired.load())
                    return;
                if (std::find(matches.begin(), matches.end(), entry.texture) == matches.end())
                    matches.push_back(entry.texture);
            });

        std::vector<std::shared_ptr<Texture>> readbackSources;
        for (auto &texture : matches)
            if (!separateGroup || texture->aliasGroup.load() != separateGroup)
                readbackSources.push_back(texture);
        SynchronizeGuestOverlaps(std::move(readbackSources), tag);

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
        auto firstChanged{textures.size()};
        for (auto it{texture->guest->mappings.begin()}; it != texture->guest->mappings.end(); ++it) {
            auto mapping{std::upper_bound(textures.begin(), textures.end(), reinterpret_cast<uintptr_t>(it->data()),
                [](uintptr_t address, const TextureMapping &entry) { return address < reinterpret_cast<uintptr_t>(entry.data()); })};
            firstChanged = std::min(firstChanged, static_cast<size_t>(mapping - textures.begin()));
            textures.emplace(mapping, TextureMapping{texture, it, *it});
        }
        overlaps.Update(textures, firstChanged);

        if (!tag) texture->ReconcileAliases();
        return texture->GetView(guestTexture.viewType, requestedRange, guestTexture.format, guestTexture.swizzle, tag);
    }
}
