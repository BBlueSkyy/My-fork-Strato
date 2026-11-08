// SPDX-License-Identifier: MPL-2.0
// Included in texture.cpp inside skyline::gpu; kept separate from image/view code.

    std::vector<texture::PhysicalImageSpan> Texture::PhysicalImageSpans(const GuestTexture &value) {
        if (!IsGuestLayoutReversible(value) || !value.MappingsValid()) return {};
        const auto levels{texture::GetBlockLinearMipLayout(value.dimensions,
            value.format->blockHeight, value.format->blockWidth, value.format->bpb,
            value.format->blockHeight, value.format->blockWidth, value.format->bpb,
            value.tileConfig.blockHeight, 1, value.mipLevelCount, value.tileConfig.tileWidthSpacing)};
        const size_t stride{value.layerStride ? value.layerStride : value.CalculateLayerSize()};
        std::vector<texture::PhysicalImageSpan> spans;
        for (u32 layer{}; layer < value.layerCount; ++layer) {
            size_t levelOffset{layer * stride};
            for (u32 level{}; level < levels.size(); ++level) {
                const auto &layout{levels[level]};
                const size_t widthBytes{util::DivideCeil<size_t>(layout.dimensions.width, value.format->blockWidth) * value.format->bpb};
                const size_t rowHeight{8 * layout.blockHeight * value.format->blockHeight};
                const size_t rowBytes{widthBytes * 8 * layout.blockHeight};
                // Dense rows have no GOB/mip padding that another layout could
                // interpret as pixels. No guessed authority over padding bytes.
                if (layout.blockLinearSize != layout.linearSize || widthBytes % 64 ||
                    layout.dimensions.height % rowHeight || layout.dimensions.depth != 1)
                    return {};
                size_t mappingOffset{};
                for (const auto &mapping : value.mappings) {
                    const size_t first{std::max(mappingOffset, levelOffset)};
                    const size_t last{std::min(mappingOffset + mapping.size(), levelOffset + layout.blockLinearSize)};
                    if (first < last) {
                        if ((first - levelOffset) % rowBytes || (last - first) % rowBytes) return {};
                        const auto begin{reinterpret_cast<uintptr_t>(mapping.data()) + first - mappingOffset};
                        spans.push_back({begin, begin + last - first, first, rowBytes, level, layer,
                            static_cast<u32>((first - levelOffset) / rowBytes * rowHeight), static_cast<u32>(rowHeight)});
                    }
                    mappingOffset += mapping.size();
                }
                levelOffset += layout.blockLinearSize;
            }
        }
        auto sorted{spans};
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
        for (size_t i{1}; i < sorted.size(); ++i)
            if (sorted[i - 1].end > sorted[i].begin) return {}; // Self-aliasing mappings need a separate policy.
        return spans;
    }

    bool Texture::CanDiscardGuest(const GuestTexture &value, size_t mappedSize) {
        const auto spans{PhysicalImageSpans(value)};
        size_t restored{};
        for (const auto &span : spans) {
            if (span.offset != restored) return false;
            restored += span.end - span.begin;
        }
        return restored && restored == mappedSize;
    }

    bool Texture::RegisterRegionAuthority(std::vector<std::shared_ptr<Texture>> members, ContextTag context) {
        if (members.size() < 2) return false;
        // Expand existing components before partitioning; disjoint neighbors of
        // an intersecting member retain their independent writer histories.
        for (size_t i{}; i < members.size(); ++i)
            if (auto group{members[i]->regionAuthority.load()}) {
                std::scoped_lock metadata{group->mutex};
                for (const auto &weak : group->members)
                    if (auto member{weak.lock()}; member && std::find(members.begin(), members.end(), member) == members.end())
                        members.push_back(std::move(member));
            }
        if (context && std::any_of(members.begin(), members.end(), [&](const auto &member) { return member->tag.load() == context; }))
            throw TextureAliasReconciliationRequest{{}, {}, {}, {}, members};
        std::sort(members.begin(), members.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        members.erase(std::unique(members.begin(), members.end()), members.end());
        std::vector<ContextLock<Texture>> locks;
        locks.reserve(members.size());
        for (const auto &member : members) locks.emplace_back(context, *member);
        std::vector<std::unique_lock<std::recursive_mutex>> stateLocks;
        stateLocks.reserve(members.size());
        for (const auto &member : members) stateLocks.emplace_back(member->stateMutex);
        std::vector<std::vector<texture::PhysicalImageSpan>> spans;
        std::vector<uintptr_t> endpoints;
        const auto &reference{*members.front()->guest};
        for (const auto &member : members) {
            if (!member->guest || member->aliasGroup.load() || member->aliasReadOnly.load() || member->aliasRetired.load() ||
                member->format != member->guest->format || member->format->vkFormat != reference.format->vkFormat ||
                member->guest->dimensions != reference.dimensions || member->guest->tileConfig != reference.tileConfig ||
                member->guest->mipLevelCount != reference.mipLevelCount || member->guest->layerCount != reference.layerCount ||
                member->tiling != vk::ImageTiling::eOptimal || member->sampleCount != vk::SampleCountFlagBits::e1)
                return false;
            spans.push_back(PhysicalImageSpans(*member->guest));
            if (spans.back().empty()) return false;
            for (const auto &span : spans.back()) { endpoints.push_back(span.begin); endpoints.push_back(span.end); }
        }
        std::sort(endpoints.begin(), endpoints.end());
        endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        auto group{std::make_shared<texture::RegionAuthority>()};
        // Hold all old metadata locks only after resource acquisition. Traps
        // never block on these locks while owning trapMutex.
        std::vector<std::shared_ptr<texture::RegionAuthority>> previous;
        std::vector<std::unique_lock<std::recursive_mutex>> metadataLocks;
        for (const auto &member : members)
            if (auto old{member->regionAuthority.load()}; old && std::find(previous.begin(), previous.end(), old) == previous.end()) {
                previous.push_back(old); metadataLocks.emplace_back(old->mutex);
                for (const auto &region : old->regions) { endpoints.push_back(region.begin); endpoints.push_back(region.end); }
            }
        std::sort(endpoints.begin(), endpoints.end());
        endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        std::vector<std::vector<texture::PhysicalImageBinding>> bindings(members.size());
        for (size_t i{1}; i < endpoints.size(); ++i) {
            const auto begin{endpoints[i - 1]}, end{endpoints[i]};
            bool covered{};
            size_t rowBytes{}, rowHeight{};
            for (size_t j{}; j < members.size(); ++j)
                for (const auto &span : spans[j]) if (span.begin <= begin && span.end >= end) {
                    if ((begin - span.begin) % span.rowBytes || (end - begin) % span.rowBytes ||
                        (covered && (rowBytes != span.rowBytes || rowHeight != span.rowHeight))) return false;
                    rowBytes = span.rowBytes; rowHeight = span.rowHeight;
                    bindings[j].push_back({group->regions.size(), span.offset + begin - span.begin, span.level, span.layer,
                        span.y + static_cast<u32>((begin - span.begin) / span.rowBytes * span.rowHeight), span.rowBytes, span.rowHeight});
                    covered = true;
                }
            if (!covered) continue;
            texture::PhysicalImageRegion region{begin, end, {}, {}, {}, {}};
            for (const auto &old : previous)
                for (const auto &source : old->regions)
                    if (source.begin <= begin && source.end >= end) {
                        region = source; region.begin = begin; region.end = end;
                    }
            group->regions.push_back(std::move(region));
        }
        // All allocations precede publication. Neighbor lists are bounded by
        // actual shared regions, not the manager's texture cache.
        std::vector<std::vector<std::weak_ptr<Texture>>> neighbors(members.size());
        for (size_t i{}; i < members.size(); ++i) {
            group->members.emplace_back(members[i]);
            for (size_t j{}; j < members.size(); ++j) if (i != j) {
                bool overlap{};
                for (const auto &a : bindings[i]) for (const auto &b : bindings[j]) overlap |= a.region == b.region;
                if (overlap) neighbors[i].emplace_back(members[j]);
            }
        }
        for (size_t i{}; i < members.size(); ++i) {
            const auto old{members[i]->regionAuthority.load()};
            for (auto &binding : bindings[i]) {
                const auto &region{group->regions[binding.region]};
                binding.residentVersion = region.guestVersion;
                if (old) for (const auto &previousBinding : members[i]->regionBindings) {
                    const auto &previousRegion{old->regions[previousBinding.region]};
                    if (previousRegion.begin <= region.begin && previousRegion.end >= region.end)
                        binding.residentVersion = previousBinding.residentVersion;
                }
            }
            members[i]->regionBindings = std::move(bindings[i]);
            members[i]->regionNeighbors = std::move(neighbors[i]);
            members[i]->regionAuthority.store(group);
            members[i]->regionTracked.store(true, std::memory_order_relaxed);
        }
        return true;
    }

    void Texture::InvalidateRegionPeers() {
        for (const auto &weak : regionNeighbors)
            if (auto member{weak.lock()}; member && !member->regionStale.exchange(true, std::memory_order_relaxed))
                ++texture::regionInvalidationSequence;
    }

    void Texture::RecordRegionWrite() {
        auto group{regionAuthority.load()};
        std::scoped_lock metadata{group->mutex};
        for (auto &binding : regionBindings) {
            auto &region{group->regions[binding.region]};
            binding.residentVersion = gpuWriteSerial;
            region.writerIdentity = this;
            region.writer = weak_from_this();
            region.hostVersion = gpuWriteSerial;
        }
        InvalidateRegionPeers();
    }

    bool Texture::NeedsRegionReadback() const {
        auto group{regionAuthority.load()};
        for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (region.writerIdentity == this && region.hostVersion != region.guestVersion) return true;
        }
        return false;
    }

    void Texture::PublishRegionGuest(span<const u8> encoded) {
        auto group{regionAuthority.load()};
        for (const auto &binding : regionBindings) {
            auto &region{group->regions[binding.region]};
            if (region.writerIdentity != this || region.hostVersion == region.guestVersion) continue;
            const size_t size{region.end - region.begin};
            std::memcpy(mirror.data() + binding.offset, encoded.data() + binding.offset, size);
            region.guestVersion = region.hostVersion;
        }
    }

    void Texture::ReleaseRegionHost() {
        auto group{regionAuthority.load()};
        for (const auto &binding : regionBindings) {
            auto &region{group->regions[binding.region]};
            // Other owner callbacks must still publish before the CPU fault is
            // released. Relinquishing foreign cells here would lose their writes.
            if (!region.writerIdentity || region.writerIdentity == this) {
                region.writerIdentity = nullptr; region.writer.reset();
                region.hostVersion = region.guestVersion = ++texture::gpuWriteSequence;
            }
        }
        // A CPU-dirty upload may need to restore foreign GPU-owned rows after
        // taking the guest snapshot. Enter the same preflight as stale peers.
        if (!regionStale.exchange(true, std::memory_order_relaxed)) ++texture::regionInvalidationSequence;
        InvalidateRegionPeers();
    }

    void Texture::RecordRegionUpload() {
        auto group{regionAuthority.load()};
        for (auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            binding.residentVersion = region.guestVersion;
            if (binding.residentVersion != region.hostVersion && !regionStale.exchange(true, std::memory_order_relaxed))
                ++texture::regionInvalidationSequence;
        }
    }

    void Texture::ReconcileRegions() {
      for (;;) {
        auto group{regionAuthority.load()};
        if (!group || !regionStale.load(std::memory_order_relaxed)) return;
        std::vector<std::shared_ptr<Texture>> resources{shared_from_this()};
        {
            std::scoped_lock metadata{group->mutex};
            if (regionAuthority.load() != group) continue;
            for (const auto &binding : regionBindings) {
                const auto &region{group->regions[binding.region]};
                if (!region.writerIdentity || region.writerIdentity == this || binding.residentVersion == region.hostVersion) continue;
                if (auto source{region.writer.lock()}; source && std::find(resources.begin(), resources.end(), source) == resources.end())
                    resources.push_back(std::move(source));
            }
        }
        std::sort(resources.begin(), resources.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        std::vector<std::unique_lock<Texture>> locks;
        locks.reserve(resources.size());
        for (const auto &resource : resources) locks.emplace_back(*resource);
        std::unique_lock metadata{group->mutex};
        bool donorsValid{regionAuthority.load() == group};
        if (donorsValid) for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (!region.writerIdentity || region.writerIdentity == this || binding.residentVersion == region.hostVersion) continue;
            if (std::none_of(resources.begin(), resources.end(), [&](const auto &resource) { return resource.get() == region.writerIdentity; }))
                donorsValid = false;
        }
        if (!donorsValid) {
            // A writer/component may have changed while resource acquisition
            // waited. Never touch a new donor's backing or cycle without its lock.
            metadata.unlock();
            continue;
        }
        // A CPU-dirty target has no unpublished owned cells and can initialize
        // normally. A still-GPU-dirty target must retain its exclusive host rows.
        const bool initializedGuest{dirtyState == DirtyState::CpuDirty};
        SynchronizeHost(false);
        std::vector<vk::BufferImageCopy> uploads;
        if (!initializedGuest) {
            const auto packed{GetBufferImageCopies()};
            for (const auto &binding : regionBindings) {
                const auto &region{group->regions[binding.region]};
                if (region.writerIdentity || binding.residentVersion == region.hostVersion) continue;
                const auto &mip{mipLayouts[binding.level]};
                const auto base{std::find_if(packed.begin(), packed.end(), [&](const auto &copy) {
                    return copy.imageSubresource.mipLevel == binding.level;
                })};
                const auto widthBytes{util::DivideCeil<size_t>(mip.dimensions.width, format->blockWidth) * format->bpb};
                uploads.push_back(vk::BufferImageCopy{
                    .bufferOffset = base->bufferOffset + binding.layer * mip.targetLinearSize + (binding.y / format->blockHeight) * widthBytes,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, binding.level, binding.layer, 1},
                    .imageOffset = {0, static_cast<i32>(binding.y), 0},
                    .imageExtent = {mip.dimensions.width,
                        static_cast<u32>((region.end - region.begin) / binding.rowBytes * binding.rowHeight), 1},
                });
            }
        }
        // Only these guest-owned rows will be uploaded. The encoder's full
        // snapshot never overwrites the target's still-authoritative GPU rows.
        auto staging{uploads.empty() ? std::shared_ptr<memory::StagingBuffer>{} : SynchronizeHostImpl()};
        struct Transfer { std::shared_ptr<Texture> source; std::vector<vk::ImageCopy> copies; };
        std::vector<Transfer> transfers;
        for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (!region.writerIdentity || region.writerIdentity == this || binding.residentVersion == region.hostVersion) continue;
            auto source{region.writer.lock()};
            if (!source) throw exception("Physical texture region lost its writer");
            auto transfer{std::find_if(transfers.begin(), transfers.end(), [&](const auto &entry) { return entry.source == source; })};
            if (transfer == transfers.end()) { transfers.push_back({source, {}}); transfer = std::prev(transfers.end()); }
            const auto origin{std::find_if(source->regionBindings.begin(), source->regionBindings.end(), [&](const auto &entry) { return entry.region == binding.region; })};
            if (origin == source->regionBindings.end()) throw exception("Physical texture region lost its source binding");
            transfer->copies.push_back(vk::ImageCopy{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, origin->level, origin->layer, 1},
                .srcOffset = {0, static_cast<i32>(origin->y), 0},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, binding.level, binding.layer, 1},
                .dstOffset = {0, static_cast<i32>(binding.y), 0},
                .extent = {std::max(1U, dimensions.width >> binding.level),
                    static_cast<u32>((region.end - region.begin) / binding.rowBytes * binding.rowHeight), 1},
            });
        }
        if (!transfers.empty() || !uploads.empty()) {
            // Wait for queue submission only, never global/device completion.
            if (cycle) cycle->WaitSubmit();
            for (const auto &transfer : transfers) if (transfer.source->cycle) transfer.source->cycle->WaitSubmit();
            auto commands{gpu.scheduler.AllocateCommandBuffer()};
            auto copyCycle{commands.GetFenceCycle()};
            try {
                copyCycle->AttachObject(shared_from_this());
                if (staging) copyCycle->AttachObject(staging);
                copyCycle->ChainCycle(cycle);
                for (const auto &transfer : transfers) {
                    copyCycle->AttachObject(transfer.source);
                    copyCycle->ChainCycle(transfer.source->cycle);
                }
                commands->begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
                if (!uploads.empty()) {
                    vk::ImageMemoryBarrier before{
                        .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .oldLayout = layout, .newLayout = vk::ImageLayout::eTransferDstOptimal,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetBacking(), .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, levelCount, 0, layerCount},
                    };
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
                    commands->copyBufferToImage(staging->vkBuffer, GetBacking(), vk::ImageLayout::eTransferDstOptimal, uploads);
                    auto after{before};
                    after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
                    after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                    after.oldLayout = before.newLayout; after.newLayout = layout;
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
                }
                for (const auto &transfer : transfers) {
                    const auto &source{transfer.source};
                    std::array<vk::ImageMemoryBarrier, 2> before{};
                    before[0].srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                    before[0].dstAccessMask = vk::AccessFlagBits::eTransferRead;
                    before[0].oldLayout = source->layout; before[0].newLayout = vk::ImageLayout::eTransferSrcOptimal;
                    before[0].image = source->GetBacking();
                    before[1].srcAccessMask = before[0].srcAccessMask;
                    before[1].dstAccessMask = vk::AccessFlagBits::eTransferWrite;
                    before[1].oldLayout = layout; before[1].newLayout = vk::ImageLayout::eTransferDstOptimal;
                    before[1].image = GetBacking();
                    for (auto &barrier : before) {
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, levelCount, 0, layerCount};
                    }
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
                    commands->copyImage(source->GetBacking(), vk::ImageLayout::eTransferSrcOptimal, GetBacking(), vk::ImageLayout::eTransferDstOptimal, transfer.copies);
                    auto after{before};
                    for (size_t i{}; i < after.size(); ++i) {
                        after[i].srcAccessMask = i ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eTransferRead;
                        after[i].dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                        after[i].oldLayout = before[i].newLayout; after[i].newLayout = before[i].oldLayout;
                    }
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
                }
                commands->end();
                gpu.scheduler.SubmitCommandBuffer(*commands, copyCycle);
            } catch (...) { copyCycle->Cancel(); throw; }
            cycle = copyCycle;
            // Keep each source's last reader alive, including retired backings.
            for (const auto &transfer : transfers) transfer.source->cycle = copyCycle;
        }
        for (auto &binding : regionBindings)
            binding.residentVersion = group->regions[binding.region].hostVersion;
        regionStale.store(false, std::memory_order_relaxed);
        return;
      }
    }

    void Texture::DrainRegionAuthority(std::vector<std::shared_ptr<Texture>> members, ContextTag context) {
        for (size_t i{}; i < members.size(); ++i)
            if (auto group{members[i]->regionAuthority.load()}) {
                std::scoped_lock metadata{group->mutex};
                for (const auto &weak : group->members)
                    if (auto member{weak.lock()}; member && std::find(members.begin(), members.end(), member) == members.end()) members.push_back(std::move(member));
            }
        if (context && std::any_of(members.begin(), members.end(), [&](const auto &member) { return member->tag.load() == context; }))
            throw TextureAliasReconciliationRequest{{}, {}, {}, {}, members};
        TextureManager::SynchronizeGuestOverlaps(members, context);
        std::sort(members.begin(), members.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        members.erase(std::unique(members.begin(), members.end()), members.end());
        std::vector<ContextLock<Texture>> locks;
        locks.reserve(members.size());
        for (const auto &member : members) locks.emplace_back(context, *member);
        for (const auto &member : members) if (auto group{member->regionAuthority.load()}) {
            std::scoped_lock state{member->stateMutex};
            std::scoped_lock metadata{group->mutex};
            member->dirtyState = DirtyState::CpuDirty;
            ++texture::regionInvalidationSequence;
            member->regionTracked = false; member->regionStale = false;
            member->regionAuthority.store({});
            member->regionBindings.clear(); member->regionNeighbors.clear();
        }
    }
