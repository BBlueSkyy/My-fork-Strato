// SPDX-License-Identifier: MPL-2.0
// Included in texture.cpp inside skyline::gpu; kept separate from image/view code.

    std::vector<texture::PhysicalImageSpan> Texture::PhysicalImageSpans(const GuestTexture &value, bool gobGranularity) {
        // Exact-alias eligibility deliberately remains separate. Planar 3D has
        // an invertible encoder too, but never infer interleaved Z from 2D rows.
        if (!CanMaterializeGuestLayout(value) || !value.MappingsValid() ||
            (value.GetImageType() != vk::ImageType::e2D && value.GetImageType() != vk::ImageType::e3D) ||
            value.tileConfig.mode != texture::TileMode::Block || value.tileConfig.blockDepth != 1) return {};
        const auto levels{texture::GetBlockLinearMipLayout(value.dimensions,
            value.format->blockHeight, value.format->blockWidth, value.format->bpb,
            value.format->blockHeight, value.format->blockWidth, value.format->bpb,
            value.tileConfig.blockHeight, 1, value.mipLevelCount, value.tileConfig.tileWidthSpacing)};
        const size_t stride{value.layerStride ? value.layerStride : value.CalculateLayerSize()};
        std::vector<texture::PhysicalImageSpan> spans;
        auto append = [&](size_t offset, size_t size, size_t rowBytes, u32 level, u32 layer,
                          u32 x, u32 y, u32 z, u32 width, u32 rowHeight) {
            size_t mappingOffset{}, covered{};
            for (const auto &mapping : value.mappings) {
                const size_t first{std::max(mappingOffset, offset)};
                const size_t last{std::min(mappingOffset + mapping.size(), offset + size)};
                if (first < last) {
                    if ((first - offset) % rowBytes || (last - first) % rowBytes) return false;
                    const auto begin{reinterpret_cast<uintptr_t>(mapping.data()) + first - mappingOffset};
                    if (last - first > UINTPTR_MAX - begin) return false;
                    spans.push_back({begin, begin + last - first, first, rowBytes, level, layer,
                        y + static_cast<u32>((first - offset) / rowBytes * rowHeight), rowHeight, x, z, width});
                    covered += last - first;
                }
                mappingOffset += mapping.size();
            }
            return covered == size;
        };
        for (u32 layer{}; layer < value.layerCount; ++layer) {
            size_t levelOffset{layer * stride};
            for (u32 level{}; level < levels.size(); ++level) {
                const auto &layout{levels[level]};
                const size_t widthBytes{util::DivideCeil<size_t>(layout.dimensions.width, value.format->blockWidth) * value.format->bpb};
                const size_t rows{util::DivideCeil<size_t>(layout.dimensions.height, value.format->blockHeight)};
                if (!gobGranularity) {
                    const size_t rowHeight{8 * layout.blockHeight * value.format->blockHeight};
                    const size_t rowBytes{widthBytes * 8 * layout.blockHeight};
                    if (layout.blockLinearSize != layout.linearSize || widthBytes % 64 || layout.dimensions.height % rowHeight)
                        return {};
                    for (u32 z{}; z < layout.dimensions.depth; ++z)
                        if (!append(levelOffset + z * layout.blockLinearSize / layout.dimensions.depth,
                            layout.blockLinearSize / layout.dimensions.depth, rowBytes, level, layer,
                            0, 0, z, layout.dimensions.width, static_cast<u32>(rowHeight))) return {};
                } else {
                    // Whole GOBs only: arbitrary byte/sector endpoints and
                    // partially represented edge GOBs keep the existing drain.
                    if (widthBytes % 64 || rows % 8 || 64 % value.format->bpb ||
                        layout.dimensions.width % value.format->blockWidth || layout.dimensions.height % value.format->blockHeight)
                        return {};
                    const size_t count{(widthBytes / 64) * (rows / 8) * layout.dimensions.depth};
                    // Bound setup, reconciliation and publication metadata for
                    // the fallback; dense rows have no new size restriction.
                    if (count > 4096 || spans.size() > 4096 - count) return {};
                    const size_t columns{texture::GetBlockLinearWidthGobs(layout.dimensions,
                        value.format->blockWidth, value.format->blockHeight, value.format->bpb,
                        layout.blockHeight, 1, layout.tileWidthSpacing)};
                    const size_t blockRows{util::DivideCeil<size_t>(rows, 8 * layout.blockHeight)};
                    for (u32 z{}; z < layout.dimensions.depth; ++z)
                        for (size_t y{}; y < rows; y += 8)
                            for (size_t x{}; x < widthBytes; x += 64) {
                                const size_t gob{((z * blockRows + y / (8 * layout.blockHeight)) * columns + x / 64) * layout.blockHeight + (y / 8) % layout.blockHeight};
                                if (!append(levelOffset + gob * 512, 512, 512, level, layer,
                                    static_cast<u32>(x / value.format->bpb * value.format->blockWidth),
                                    static_cast<u32>(y * value.format->blockHeight), z,
                                    static_cast<u32>(64 / value.format->bpb * value.format->blockWidth),
                                    static_cast<u32>(8 * value.format->blockHeight))) return {};
                            }
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
        std::vector<std::shared_ptr<Buffer>> buffers;
        for (const auto &member : members) if (auto group{member->regionAuthority.load()}) {
            std::scoped_lock metadata{group->mutex};
            for (const auto &weak : group->buffers) if (auto buffer{weak.lock()}; buffer && std::find(buffers.begin(), buffers.end(), buffer) == buffers.end()) buffers.push_back(buffer);
        }
        if (!buffers.empty()) return RegisterMixedRegionAuthority(members, buffers, context);
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
        if (!members.front()->guest) return false;
        const auto &reference{*members.front()->guest};
        bool gobGranularity{};
        for (const auto &member : members) {
            if (!member->guest || member->aliasGroup.load() || member->aliasReadOnly.load() || member->aliasRetired.load() ||
                member->format != member->guest->format || member->format->vkFormat != reference.format->vkFormat ||
                member->tiling != vk::ImageTiling::eOptimal || member->sampleCount != vk::SampleCountFlagBits::e1)
                return false;
            spans.push_back(PhysicalImageSpans(*member->guest));
            gobGranularity |= spans.back().empty() || member->guest->dimensions != reference.dimensions ||
                member->guest->tileConfig != reference.tileConfig || member->guest->mipLevelCount != reference.mipLevelCount ||
                member->guest->layerCount != reference.layerCount;
        }
        // A physical overlap may cut a dense ROB at a GOB boundary even
        // when both layouts are otherwise identical. Try the finer proof only
        // when a dense partition boundary actually requires it.
        if (!gobGranularity) {
            for (const auto &memberSpans : spans)
                for (const auto &span : memberSpans) { endpoints.push_back(span.begin); endpoints.push_back(span.end); }
            std::sort(endpoints.begin(), endpoints.end());
            endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
            for (const auto &memberSpans : spans)
                for (const auto &span : memberSpans)
                    for (auto point{std::upper_bound(endpoints.begin(), endpoints.end(), span.begin)};
                         point != endpoints.end() && *point < span.end; ++point)
                        gobGranularity |= (*point - span.begin) % span.rowBytes != 0;
        }
        if (gobGranularity) {
            spans.clear(); endpoints.clear();
            for (const auto &member : members) spans.push_back(PhysicalImageSpans(*member->guest, true));
        }
        for (auto &memberSpans : spans) {
            if (memberSpans.empty()) return false;
            std::sort(memberSpans.begin(), memberSpans.end(), [](const auto &a, const auto &b) { return a.begin < b.begin; });
            if (gobGranularity)
                for (const auto &span : memberSpans) { endpoints.push_back(span.begin); endpoints.push_back(span.end); }
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
            size_t rowBytes{}, rowHeight{}, width{};
            for (size_t j{}; j < members.size(); ++j) {
                const auto candidate{std::upper_bound(spans[j].begin(), spans[j].end(), begin,
                    [](uintptr_t address, const auto &span) { return address < span.begin; })};
                if (candidate == spans[j].begin()) continue;
                const auto &span{*std::prev(candidate)};
                if (span.begin <= begin && span.end >= end) {
                    if ((begin - span.begin) % span.rowBytes || (end - begin) % span.rowBytes ||
                        (covered && (rowBytes != span.rowBytes || rowHeight != span.rowHeight || width != span.width))) return false;
                    rowBytes = span.rowBytes; rowHeight = span.rowHeight; width = span.width;
                    bindings[j].push_back({group->regions.size(), span.offset + begin - span.begin, span.level, span.layer,
                        span.y + static_cast<u32>((begin - span.begin) / span.rowBytes * span.rowHeight), span.rowBytes, span.rowHeight, 0, span.x, span.z, span.width});
                    covered = true;
                }
            }
            if (!covered) continue;
            texture::PhysicalImageRegion region{begin, end, {}, {}, {}, {}};
            for (const auto &old : previous) {
                const auto candidate{std::upper_bound(old->regions.begin(), old->regions.end(), begin,
                    [](uintptr_t address, const auto &entry) { return address < entry.begin; })};
                if (candidate != old->regions.begin()) {
                    const auto &source{*std::prev(candidate)};
                    if (source.end >= end) { region = source; region.begin = begin; region.end = end; }
                }
            }
            group->regions.push_back(std::move(region));
        }
        // All allocations precede publication. Neighbor lists are bounded by
        // actual shared regions, not the manager's texture cache.
        std::vector<std::vector<std::weak_ptr<Texture>>> neighbors(members.size());
        for (size_t i{}; i < members.size(); ++i) {
            group->members.emplace_back(members[i]);
            for (size_t j{}; j < members.size(); ++j) if (i != j) {
                auto a{bindings[i].begin()}, b{bindings[j].begin()};
                while (a != bindings[i].end() && b != bindings[j].end()) {
                    if (a->region == b->region) { neighbors[i].emplace_back(members[j]); break; }
                    if (a->region < b->region) ++a; else ++b;
                }
            }
        }
        for (size_t i{}; i < members.size(); ++i) {
            const auto old{members[i]->regionAuthority.load()};
            for (auto &binding : bindings[i]) {
                const auto &region{group->regions[binding.region]};
                binding.residentVersion = region.guestVersion;
                if (old) {
                    const auto candidate{std::upper_bound(old->regions.begin(), old->regions.end(), region.begin,
                        [](uintptr_t address, const auto &entry) { return address < entry.begin; })};
                    if (candidate != old->regions.begin()) {
                        const auto previousRegion{std::prev(candidate)};
                        const size_t previousIndex{static_cast<size_t>(previousRegion - old->regions.begin())};
                        const auto &oldBindings{members[i]->regionBindings};
                        const auto source{std::lower_bound(oldBindings.begin(), oldBindings.end(), previousIndex,
                            [](const auto &entry, size_t index) { return entry.region < index; })};
                        if (previousRegion->end >= region.end && source != oldBindings.end() && source->region == previousIndex)
                            binding.residentVersion = source->residentVersion;
                    }
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
        if (auto group{regionAuthority.load()}) for (const auto &weak : group->buffers)
            if (auto member{weak.lock()}; member && !member->regionStale.exchange(true, std::memory_order_relaxed)) ++texture::regionInvalidationSequence;
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
            region.bufferWriterIdentity = {}; region.bufferWriter.reset();
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
        std::scoped_lock metadata{group->mutex};
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
            if (group->transport == texture::RegionTransport::GuestBridge ||
                !region.HasWriter() || region.writerIdentity == this) {
                region.writerIdentity = nullptr; region.writer.reset();
                region.hostVersion = region.guestVersion = ++texture::gpuWriteSequence;
            }
        }
        // A CPU-dirty upload may need to restore foreign GPU-owned rows after
        // taking the guest snapshot. Enter the same preflight as stale peers.
        if (!regionStale.exchange(true, std::memory_order_relaxed)) ++texture::regionInvalidationSequence;
        InvalidateRegionPeers();
    }

    void Texture::RecordRegionUpload(span<const uint64_t> guestVersions) {
        auto group{regionAuthority.load()};
        size_t index{};
        for (auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            binding.residentVersion = guestVersions.empty() ? region.guestVersion : guestVersions[index++];
            if (binding.residentVersion != region.hostVersion && !regionStale.exchange(true, std::memory_order_relaxed))
                ++texture::regionInvalidationSequence;
        }
    }

    void Texture::ReconcileRegions() {
        if (auto group{regionAuthority.load()}; group && group->transport == texture::RegionTransport::GuestBridge) {
            ReconcileGuestBridgeRegions();
            return;
        }
      for (;;) {
        auto group{regionAuthority.load()};
        if (!group || !regionStale.load(std::memory_order_relaxed)) return;
        std::vector<std::shared_ptr<Texture>> resources{shared_from_this()};
        std::vector<std::shared_ptr<Buffer>> bufferResources;
        {
            std::scoped_lock metadata{group->mutex};
            if (regionAuthority.load() != group) continue;
            for (const auto &binding : regionBindings) {
                const auto &region{group->regions[binding.region]};
                if (region.bufferWriterIdentity && binding.residentVersion != region.hostVersion)
                    if (auto source{region.bufferWriter.lock()}; source && std::find(bufferResources.begin(), bufferResources.end(), source) == bufferResources.end()) bufferResources.push_back(source);
                if (!region.writerIdentity || region.writerIdentity == this || binding.residentVersion == region.hostVersion) continue;
                if (auto source{region.writer.lock()}; source && std::find(resources.begin(), resources.end(), source) == resources.end())
                    resources.push_back(std::move(source));
            }
        }
        std::sort(resources.begin(), resources.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        std::sort(bufferResources.begin(), bufferResources.end(), [](const auto &a, const auto &b) { return std::less<Buffer *>{}(a.get(), b.get()); });
        std::vector<std::unique_lock<Buffer>> bufferLocks;
        for (const auto &buffer : bufferResources) bufferLocks.emplace_back(*buffer);
        std::vector<std::unique_lock<Texture>> locks;
        locks.reserve(resources.size());
        for (const auto &resource : resources) locks.emplace_back(*resource);
        std::vector<std::unique_lock<std::recursive_mutex>> stateLocks;
        stateLocks.reserve(resources.size());
        for (const auto &resource : resources) stateLocks.emplace_back(resource->stateMutex);
        for (const auto &buffer : bufferResources) buffer->SynchronizeHost();
        std::unique_lock metadata{group->mutex};
        bool donorsValid{regionAuthority.load() == group};
        if (donorsValid) for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (region.bufferWriterIdentity && binding.residentVersion != region.hostVersion &&
                std::none_of(bufferResources.begin(), bufferResources.end(), [&](const auto &resource) { return resource.get() == region.bufferWriterIdentity; })) donorsValid = false;
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
        metadata.unlock();
        SynchronizeHost(false);
        metadata.lock();
        // A different peer may have become a writer while metadata was released.
        // Re-acquire that producer before taking a new transfer snapshot.
        donorsValid = regionAuthority.load() == group;
        if (donorsValid) for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (region.writerIdentity && region.writerIdentity != this && binding.residentVersion != region.hostVersion &&
                std::none_of(resources.begin(), resources.end(), [&](const auto &resource) { return resource.get() == region.writerIdentity; }))
                donorsValid = false;
        }
        if (!donorsValid) { metadata.unlock(); continue; }
        std::vector<vk::BufferImageCopy> uploads;
        if (!initializedGuest) {
            const auto packed{GetBufferImageCopies()};
            for (const auto &binding : regionBindings) {
                const auto &region{group->regions[binding.region]};
                if (region.HasWriter() || binding.residentVersion == region.hostVersion) continue;
                const auto &mip{mipLayouts[binding.level]};
                const auto base{std::find_if(packed.begin(), packed.end(), [&](const auto &copy) {
                    return copy.imageSubresource.mipLevel == binding.level;
                })};
                const auto widthBytes{util::DivideCeil<size_t>(mip.dimensions.width, format->blockWidth) * format->bpb};
                uploads.push_back(vk::BufferImageCopy{
                    .bufferOffset = base->bufferOffset + binding.layer * mip.targetLinearSize +
                        (binding.z * util::DivideCeil<size_t>(mip.dimensions.height, format->blockHeight) + binding.y / format->blockHeight) * widthBytes +
                        (binding.x / format->blockWidth) * format->bpb,
                    .bufferRowLength = static_cast<u32>(util::DivideCeil<size_t>(mip.dimensions.width, format->blockWidth) * format->blockWidth),
                    .bufferImageHeight = static_cast<u32>(util::DivideCeil<size_t>(mip.dimensions.height, format->blockHeight) * format->blockHeight),
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, binding.level, binding.layer, 1},
                    .imageOffset = {static_cast<i32>(binding.x), static_cast<i32>(binding.y), static_cast<i32>(binding.z)},
                    .imageExtent = {binding.width,
                        static_cast<u32>((region.end - region.begin) / binding.rowBytes * binding.rowHeight), 1},
                });
            }
        }
        // Only these guest-owned rows will be uploaded. The encoder's full
        // snapshot never overwrites the target's still-authoritative GPU rows.

        struct BufferTransfer { std::shared_ptr<Buffer> source; std::vector<texture::RawRgba8Gob> gobs; std::vector<vk::BufferImageCopy> copies; };
        std::vector<BufferTransfer> bufferTransfers;
        for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (!region.bufferWriterIdentity || binding.residentVersion == region.hostVersion) continue;
            auto source{region.bufferWriter.lock()};
            if (!source || std::find(bufferResources.begin(), bufferResources.end(), source) == bufferResources.end()) throw exception("Lost mixed buffer producer");
            auto origin{std::find_if(source->regionBindings.begin(), source->regionBindings.end(), [&](const auto &entry) { return entry.region == binding.region; })};
            if (origin == source->regionBindings.end() || region.end - region.begin != 512) throw exception("Invalid raw GOB buffer source");
            auto transfer{std::find_if(bufferTransfers.begin(), bufferTransfers.end(), [&](const auto &entry) { return entry.source == source; })};
            if (transfer == bufferTransfers.end()) { bufferTransfers.push_back({source, {}, {}}); transfer = std::prev(bufferTransfers.end()); }
            transfer->gobs.push_back({origin->offset, binding.x, binding.y});
        }
        for (auto &transfer : bufferTransfers) {
            auto copies{texture::PlanRawRgba8GobCopies(transfer.gobs, transfer.source->backing->size(), dimensions)};
            if (!copies) throw exception("Unproved raw buffer to image transfer");
            transfer.copies = std::move(*copies);
        }
        struct Transfer { std::shared_ptr<Texture> source; std::vector<vk::ImageCopy> copies; };
        std::vector<Transfer> transfers;
        for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (!region.writerIdentity || region.writerIdentity == this || binding.residentVersion == region.hostVersion) continue;
            auto source{region.writer.lock()};
            if (!source) throw exception("Physical texture region lost its writer");
            auto transfer{std::find_if(transfers.begin(), transfers.end(), [&](const auto &entry) { return entry.source == source; })};
            if (transfer == transfers.end()) { transfers.push_back({source, {}}); transfer = std::prev(transfers.end()); }
            const auto origin{std::lower_bound(source->regionBindings.begin(), source->regionBindings.end(), binding.region,
                [](const auto &entry, size_t region) { return entry.region < region; })};
            if (origin == source->regionBindings.end() || origin->region != binding.region) throw exception("Physical texture region lost its source binding");
            transfer->copies.push_back(vk::ImageCopy{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, origin->level, origin->layer, 1},
                .srcOffset = {static_cast<i32>(origin->x), static_cast<i32>(origin->y), static_cast<i32>(origin->z)},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, binding.level, binding.layer, 1},
                .dstOffset = {static_cast<i32>(binding.x), static_cast<i32>(binding.y), static_cast<i32>(binding.z)},
                .extent = {binding.width,
                    static_cast<u32>((region.end - region.begin) / binding.rowBytes * binding.rowHeight), 1},
            });
        }
        std::vector<uint64_t> versions;
        versions.reserve(regionBindings.size());
        for (const auto &binding : regionBindings) versions.push_back(group->regions[binding.region].hostVersion);
        metadata.unlock();
        auto staging{uploads.empty() ? std::shared_ptr<memory::StagingBuffer>{} : SynchronizeHostImpl()};
        if (!transfers.empty() || !bufferTransfers.empty() || !uploads.empty()) {
            // Wait for queue submission only, never global/device completion.
            if (cycle) cycle->WaitSubmit();
            for (const auto &transfer : transfers) if (transfer.source->cycle) transfer.source->cycle->WaitSubmit();
            for (const auto &transfer : bufferTransfers) if (transfer.source->cycle) transfer.source->cycle->WaitSubmit();
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
                for (const auto &transfer : bufferTransfers) { copyCycle->AttachObject(transfer.source); copyCycle->ChainCycle(transfer.source->cycle); }
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
                for (const auto &transfer : bufferTransfers) {
                    vk::BufferMemoryBarrier sourceBefore{.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .buffer = transfer.source->GetBacking(), .offset = 0, .size = VK_WHOLE_SIZE};
                    vk::ImageMemoryBarrier before{.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite, .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .oldLayout = layout, .newLayout = vk::ImageLayout::eTransferDstOptimal,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetBacking(), .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, sourceBefore, before);
                    commands->copyBufferToImage(transfer.source->GetBacking(), GetBacking(), vk::ImageLayout::eTransferDstOptimal, transfer.copies);
                    auto after{before}; after.srcAccessMask = vk::AccessFlagBits::eTransferWrite; after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                    after.oldLayout = before.newLayout; after.newLayout = before.oldLayout;
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
                    before[0].subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, source->levelCount, 0, source->layerCount};
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
            for (const auto &transfer : bufferTransfers) transfer.source->cycle = copyCycle;
        }
        metadata.lock();
        bool stale{};
        size_t index{};
        for (auto &binding : regionBindings) {
            binding.residentVersion = versions[index++];
            stale |= binding.residentVersion != group->regions[binding.region].hostVersion;
        }
        regionStale.store(stale, std::memory_order_relaxed);
        if (stale) { metadata.unlock(); continue; }
        return;
      }
    }

    void Texture::DrainRegionAuthority(std::vector<std::shared_ptr<Texture>> members, ContextTag context) {
        std::vector<std::shared_ptr<Buffer>> buffers;
        for (size_t i{}; i < members.size(); ++i)
            if (auto group{members[i]->regionAuthority.load()}) {
                std::scoped_lock metadata{group->mutex};
                for (const auto &weak : group->buffers) if (auto buffer{weak.lock()}; buffer && std::find(buffers.begin(), buffers.end(), buffer) == buffers.end()) buffers.push_back(buffer);
                for (const auto &weak : group->members)
                    if (auto member{weak.lock()}; member && std::find(members.begin(), members.end(), member) == members.end()) members.push_back(std::move(member));
            }
        if (context && (std::any_of(members.begin(), members.end(), [&](const auto &member) { return member->tag.load() == context; }) ||
            std::any_of(buffers.begin(), buffers.end(), [&](const auto &buffer) { return buffer->tag.load() == context; })))
            throw TextureAliasReconciliationRequest{{}, {}, {}, {}, members};
        // Publication is limited to the requested buffer's cells. A drain must
        // cover every member, including disjoint buffer-exclusive writers that
        // are connected only through an image or a larger alias.
        for (const auto &buffer : buffers) buffer->PublishRegionGuest();
        TextureManager::SynchronizeGuestOverlaps(members, context);
        std::sort(members.begin(), members.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        members.erase(std::unique(members.begin(), members.end()), members.end());
        std::sort(buffers.begin(), buffers.end(), [](const auto &a, const auto &b) { return std::less<Buffer *>{}(a.get(), b.get()); });
        std::vector<ContextLock<Buffer>> bufferLocks;
        for (const auto &buffer : buffers) bufferLocks.emplace_back(context, *buffer);
        std::vector<ContextLock<Texture>> locks;
        locks.reserve(members.size());
        for (const auto &member : members) locks.emplace_back(context, *member);
        // A skipped/non-reversible readback is not publication. Validate every
        // owner before detaching any member, preserving the whole old domain on
        // failure. Resource locks exclude new GPU writers during both passes.
        for (const auto &member : members) if (auto group{member->regionAuthority.load()}) {
            std::scoped_lock state{member->stateMutex};
            std::scoped_lock metadata{group->mutex};
            if (member->NeedsRegionReadback())
                throw exception("Cannot drain an unpublished physical texture writer");
        }
        for (const auto &buffer : buffers) if (auto group{buffer->regionAuthority.load()}) {
            std::scoped_lock state{buffer->stateMutex};
            std::scoped_lock metadata{group->mutex};
            if (buffer->NeedsRegionGuest()) throw exception("Cannot drain unpublished mixed buffer cells");
        }
        for (const auto &buffer : buffers) if (auto group{buffer->regionAuthority.load()}) {
            std::scoped_lock state{buffer->stateMutex}; std::scoped_lock metadata{group->mutex};
            buffer->dirtyState = Buffer::DirtyState::CpuDirty; buffer->regionTracked = false; buffer->regionStale = false;
            buffer->regionAuthority.store({}); buffer->regionBindings.clear();
        }
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
