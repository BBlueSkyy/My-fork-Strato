// SPDX-License-Identifier: MPL-2.0
// Included in texture.cpp inside skyline::gpu. This is the TRANSACTIONAL
// ADMISSION half of the staged guest bridge, not a standalone safe transport.
// Actual GPU repair/publication paths MUST dispatch by RegionTransport before
// MixedGuestBridgeExperimental may be enabled.
    bool Texture::RegisterGuestBridgeRegionAuthority(
        std::vector<std::shared_ptr<Texture>> images,
        std::vector<std::shared_ptr<Buffer>> buffers, ContextTag context) {
        if constexpr (!texture::MixedGuestBridgeExperimental) return false;
        constexpr size_t MaxCells{8192};
        constexpr size_t MaxParticipants{64};
        std::vector<std::shared_ptr<texture::RegionAuthority>> previous;
        auto expand = [&](const auto &resource) {
            if (auto group{resource->regionAuthority.load()};
                group && std::find(previous.begin(), previous.end(), group) == previous.end()) {
                previous.push_back(group);
                std::scoped_lock metadata{group->mutex};
                for (const auto &weak : group->members)
                    if (auto member{weak.lock()}; member &&
                        std::find(images.begin(), images.end(), member) == images.end())
                        images.push_back(member);
                for (const auto &weak : group->buffers)
                    if (auto member{weak.lock()}; member &&
                        std::find(buffers.begin(), buffers.end(), member) == buffers.end())
                        buffers.push_back(member);
            }
        };
        for (size_t i{}, j{}; i < images.size() || j < buffers.size();) {
            if (i < images.size()) expand(images[i++]);
            if (j < buffers.size()) expand(buffers[j++]);
            if (images.size() + buffers.size() > MaxParticipants) return false;
        }
        if (images.empty() || buffers.empty()) return false;
        if (context && (std::any_of(images.begin(), images.end(), [&](const auto &image) {
                return image->tag.load() == context;
            }) || std::any_of(buffers.begin(), buffers.end(), [&](const auto &buffer) {
                return buffer->tag.load() == context;
            }))) {
            TextureAliasReconciliationRequest request;
            request.mixedRegistrationImages = images;
            request.mixedRegistrationBuffers = buffers;
            throw request;
        }
        std::sort(buffers.begin(), buffers.end(), [](const auto &a, const auto &b) {
            return std::less<Buffer *>{}(a.get(), b.get());
        });
        std::sort(images.begin(), images.end(), [](const auto &a, const auto &b) {
            return std::less<Texture *>{}(a.get(), b.get());
        });
        std::vector<ContextLock<Buffer>> bufferLocks;
        std::vector<ContextLock<Texture>> imageLocks;
        for (const auto &buffer : buffers) bufferLocks.emplace_back(context, *buffer);
        for (const auto &image : images) imageLocks.emplace_back(context, *image);
        std::vector<std::unique_lock<RecursiveSpinLock>> bufferStates;
        std::vector<std::unique_lock<std::recursive_mutex>> imageStates;
        for (const auto &buffer : buffers) bufferStates.emplace_back(buffer->stateMutex);
        for (const auto &image : images) imageStates.emplace_back(image->stateMutex);

        // No mutating operation below is allowed before all layouts, coverage,
        // component partitions, writer versions and bindings are prepared.
        std::vector<std::vector<texture::GuestBridgePhysicalRun>> spans;
        spans.reserve(images.size());
        std::vector<uintptr_t> endpoints;
        for (const auto &image : images) {
            if (!image->guest || image->aliasGroup.load() ||
                image->aliasRetired.load() || image->aliasReadOnly.load() ||
                image->format != image->guest->format || image->memoryFreed ||
                image->tiling != vk::ImageTiling::eOptimal ||
                image->sampleCount != vk::SampleCountFlagBits::e1 ||
                !IsGuestLayoutReversible(*image->guest))
                return false;
            auto coverage{PlanGuestBridgePhysicalCoverage(*image->guest)};
            if (!coverage || coverage->empty() ||
                coverage->size() > MaxCells) return false;
            for (const auto &span : *coverage) {
                if (span.begin >= span.end ||
                    endpoints.size() + 2 > 2 * MaxCells) return false;
                endpoints.push_back(span.begin); endpoints.push_back(span.end);
            }
            spans.push_back(std::move(*coverage));
        }
        for (const auto &buffer : buffers) {
            if (!buffer->guest || buffer->isDirect ||
                !buffer->guest->data() || buffer->guest->empty() ||
                endpoints.size() + 2 > 2 * MaxCells) return false;
            const auto start{reinterpret_cast<uintptr_t>(buffer->guest->data())};
            if (buffer->guest->size() > UINTPTR_MAX - start) return false;
            endpoints.push_back(start); endpoints.push_back(start + buffer->guest->size());
        }

        std::sort(previous.begin(), previous.end(), [](const auto &a, const auto &b) {
            return std::less<texture::RegionAuthority *>{}(a.get(), b.get());
        });
        std::vector<std::unique_lock<std::recursive_mutex>> oldLocks;
        for (const auto &old : previous) {
            oldLocks.emplace_back(old->mutex);
            for (const auto &region : old->regions) {
                if (endpoints.size() + 2 > 2 * MaxCells ||
                    region.begin >= region.end) return false;
                endpoints.push_back(region.begin); endpoints.push_back(region.end);
            }
        }
        std::sort(endpoints.begin(), endpoints.end());
        endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        if (endpoints.size() < 2 || endpoints.size() - 1 > MaxCells) return false;

        auto group{std::make_shared<texture::RegionAuthority>()};
        group->transport = texture::RegionTransport::GuestBridge;
        std::vector<std::vector<texture::PhysicalImageBinding>> imageBindings(images.size());
        std::vector<std::vector<texture::PhysicalBufferBinding>> bufferBindings(buffers.size());

        for (size_t i{1}; i < endpoints.size(); ++i) {
            const auto begin{endpoints[i - 1]}, end{endpoints[i]};
            bool covered{};
            for (size_t j{}; j < images.size(); ++j) {
                const auto &runs{spans[j]};
                auto it{std::upper_bound(runs.begin(), runs.end(), begin,
                    [](uintptr_t value, const auto &run) { return value < run.begin; })};
                if (it == runs.begin()) continue;
                --it;
                if (it->begin <= begin && it->end >= end) {
                    const auto offset{it->guestOffset + (begin - it->begin)};
                    imageBindings[j].push_back({group->regions.size(), offset,
                        0, 0, 0, static_cast<size_t>(end - begin), 1, 0, 0, 0, 0});
                    covered = true;
                }
            }
            for (size_t j{}; j < buffers.size(); ++j) {
                const auto start{reinterpret_cast<uintptr_t>(buffers[j]->guest->data())};
                if (start <= begin &&
                    end - start <= buffers[j]->guest->size()) {
                    bufferBindings[j].push_back({group->regions.size(), begin - start, 0});
                    covered = true;
                }
            }
            if (!covered) {
                for (const auto &old : previous)
                    for (const auto &region : old->regions)
                        if (region.begin <= begin && region.end >= end &&
                            region.hostVersion != region.guestVersion)
                            return false; // Cannot discard an unpublished old owner.
                continue;
            }
            texture::PhysicalImageRegion region{begin, end};
            for (const auto &old : previous) {
                const auto it{std::upper_bound(old->regions.begin(), old->regions.end(), begin,
                    [](uintptr_t value, const auto &cell) { return value < cell.begin; })};
                if (it == old->regions.begin()) continue;
                const auto &prior{*std::prev(it)};
                if (prior.begin > begin || prior.end < end) continue;
                if (prior.hostVersion > region.hostVersion) {
                    region = prior; region.begin = begin; region.end = end;
                } else if (prior.hostVersion == region.hostVersion &&
                           region.hostVersion && prior.HasWriter() &&
                           region.HasWriter() &&
                           (prior.writerIdentity != region.writerIdentity ||
                            prior.bufferWriterIdentity != region.bufferWriterIdentity))
                    return false;
            }
            // Only committed GPU writers are candidates. CpuDirty/clean
            // representations are not automatically promoted to producers.
            const auto cellId{group->regions.size()};
            for (size_t j{}; j < images.size(); ++j) if (
                !imageBindings[j].empty() &&
                imageBindings[j].back().region == cellId &&
                !images[j]->regionAuthority.load() &&
                images[j]->gpuWriteSerial > images[j]->gpuReadbackSerial &&
                images[j]->gpuWriteSerial > region.hostVersion &&
                images[j]->dirtyState == DirtyState::GpuDirty) {
                    region.writerIdentity = images[j].get(); region.writer = images[j];
                    region.bufferWriterIdentity = {}; region.bufferWriter.reset();
                    region.hostVersion = images[j]->gpuWriteSerial;
            }
            for (size_t j{}; j < buffers.size(); ++j) if (
                !bufferBindings[j].empty() &&
                bufferBindings[j].back().region == cellId &&
                !buffers[j]->regionAuthority.load() &&
                buffers[j]->dirtyState == Buffer::DirtyState::GpuDirty &&
                buffers[j]->gpuWriteSerial > region.hostVersion) {
                    region.writerIdentity = {}; region.writer.reset();
                    region.bufferWriterIdentity = buffers[j].get();
                    region.bufferWriter = buffers[j];
                    region.hostVersion = buffers[j]->gpuWriteSerial;
            }
            if (region.HasWriter() && region.hostVersion != region.guestVersion) {
                const bool imageAlive{region.writerIdentity && std::any_of(
                    images.begin(), images.end(), [&](const auto &member) {
                        return member.get() == region.writerIdentity;
                    })};
                const bool bufferAlive{region.bufferWriterIdentity && std::any_of(
                    buffers.begin(), buffers.end(), [&](const auto &member) {
                        return member.get() == region.bufferWriterIdentity;
                    })};
                if (!imageAlive && !bufferAlive) return false;
            }
            group->regions.push_back(region);
        }
        if (group->regions.empty()) return false;
        auto resident = [&](const auto &resource, auto &binding) {
            const auto &region{group->regions[binding.region]};
            binding.residentVersion = region.guestVersion;
            if (auto old{resource->regionAuthority.load()})
                for (const auto &source : resource->regionBindings) {
                    const auto &prior{old->regions[source.region]};
                    if (prior.begin <= region.begin && prior.end >= region.end) {
                        binding.residentVersion = source.residentVersion;
                        break;
                    }
                }
        };
        for (size_t j{}; j < images.size(); ++j)
            for (auto &binding : imageBindings[j]) {
                resident(images[j], binding);
                const auto &region{group->regions[binding.region]};
                if (!images[j]->regionAuthority.load() &&
                    region.writerIdentity == images[j].get())
                    binding.residentVersion = region.hostVersion;
            }
        for (size_t j{}; j < buffers.size(); ++j)
            for (auto &binding : bufferBindings[j]) {
                resident(buffers[j], binding);
                const auto &region{group->regions[binding.region]};
                if (!buffers[j]->regionAuthority.load() &&
                    region.bufferWriterIdentity == buffers[j].get())
                    binding.residentVersion = region.hostVersion;
            }
        for (const auto &image : images) group->members.push_back(image);
        for (const auto &buffer : buffers) group->buffers.push_back(buffer);
        std::vector<std::vector<std::weak_ptr<Texture>>> neighbors(images.size());
        for (size_t i{}; i < images.size(); ++i)
            for (size_t j{}; j < images.size(); ++j)
                if (i != j) neighbors[i].push_back(images[j]);

        // Commit prepared member graph only after every allocation succeeds.
        for (size_t j{}; j < images.size(); ++j) {
            images[j]->regionBindings = std::move(imageBindings[j]);
            images[j]->regionNeighbors = std::move(neighbors[j]);
            images[j]->regionAuthority.store(group);
            images[j]->regionTracked = true; images[j]->regionStale = true;
            images[j]->bufferLookupRevision = images[j]->gpu.buffer.regionRevision;
        }
        for (size_t j{}; j < buffers.size(); ++j) {
            buffers[j]->regionBindings = std::move(bufferBindings[j]);
            buffers[j]->regionAuthority.store(group);
            buffers[j]->regionTracked = true; buffers[j]->regionStale = true;
            buffers[j]->regionLookupRevision = buffers[j]->gpu.texture.RegionRevision();
        }
        ++texture::regionInvalidationSequence;
        return true;
    }

    // Publish only the current producers of this image's exact physical bytes.
    // Lock donors outside metadata in Buffer->Texture address order; no GPU
    // wait or Vulkan operation is ever made under regional metadata.
    void Texture::PublishGuestBridgeRegions() {
      for (;;) {
        auto group{regionAuthority.load()};
        if (!group || group->transport != texture::RegionTransport::GuestBridge) return;
        std::vector<std::shared_ptr<Buffer>> buffers;
        std::vector<std::shared_ptr<Texture>> images;
        {
            std::scoped_lock metadata{group->mutex};
            if (regionAuthority.load() != group) continue;
            for (const auto &binding : regionBindings) {
                const auto &cell{group->regions[binding.region]};
                if (cell.guestVersion == cell.hostVersion) continue;
                if (cell.bufferWriterIdentity) {
                    auto donor{cell.bufferWriter.lock()};
                    if (!donor) throw exception("Lost guest-bridge buffer writer");
                    if (std::find(buffers.begin(), buffers.end(), donor) == buffers.end()) buffers.push_back(std::move(donor));
                } else if (cell.writerIdentity) {
                    auto donor{cell.writer.lock()};
                    if (!donor) throw exception("Lost guest-bridge texture writer");
                    if (std::find(images.begin(), images.end(), donor) == images.end()) images.push_back(std::move(donor));
                } else {
                    throw exception("Unpublished guest-bridge cell has no producer");
                }
            }
        }
        std::sort(buffers.begin(), buffers.end(), [](const auto &a, const auto &b) { return std::less<Buffer *>{}(a.get(), b.get()); });
        std::sort(images.begin(), images.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        std::vector<std::unique_lock<Buffer>> bufferLocks;
        std::vector<std::unique_lock<Texture>> imageLocks;
        for (const auto &buffer : buffers) bufferLocks.emplace_back(*buffer);
        for (const auto &image : images) imageLocks.emplace_back(*image);
        {
            std::scoped_lock metadata{group->mutex};
            if (regionAuthority.load() != group) continue;
            bool valid{true};
            for (const auto &binding : regionBindings) {
                const auto &cell{group->regions[binding.region]};
                if (cell.hostVersion == cell.guestVersion) continue;
                if (cell.bufferWriterIdentity && std::none_of(buffers.begin(), buffers.end(), [&](const auto &donor) {
                        return donor.get() == cell.bufferWriterIdentity;
                    })) valid = false;
                if (cell.writerIdentity && std::none_of(images.begin(), images.end(), [&](const auto &donor) {
                        return donor.get() == cell.writerIdentity;
                    })) valid = false;
            }
            if (!valid) continue;
        }
        // Every readback below goes through the existing region-owned masked
        // encoder/copy. The mirror is real guest memory, not scratch memory.
        for (const auto &buffer : buffers) buffer->SynchronizeGuest();
        for (const auto &image : images) image->SynchronizeGuest(false, true);
        {
            std::scoped_lock metadata{group->mutex};
            if (regionAuthority.load() != group) continue;
            for (const auto &binding : regionBindings) {
                const auto &cell{group->regions[binding.region]};
                if (cell.hostVersion != cell.guestVersion)
                    throw exception("Guest-bridge donor failed to publish physical bytes");
            }
        }
        return;
      }
    }

    void Texture::ReconcileGuestBridgeRegions() {
      for (;;) {
        auto group{regionAuthority.load()};
        if (!group || group->transport != texture::RegionTransport::GuestBridge ||
            !regionStale.load(std::memory_order_relaxed)) return;
        // A pending executor consumer still requires the previous backing.
        // Request submission/retry instead of updating it in-place.
        if (pendingRegionUseTag) {
            TextureAliasReconciliationRequest request;
            request.target = shared_from_this();
            throw request;
        }
        // Prepare all the target's needed bytes before uploading the ENTIRE
        // native subresource. Include unpublished exclusive bytes owned by the
        // target itself; otherwise a whole-image upload would erase them.
        PublishGuestBridgeRegions();
        std::unique_lock resource{*this};
        std::unique_lock state{stateMutex};
        if (regionAuthority.load() != group) continue;
        {
            std::scoped_lock metadata{group->mutex};
            for (const auto &binding : regionBindings) {
                const auto &cell{group->regions[binding.region]};
                if (cell.hostVersion != cell.guestVersion)
                    throw exception("Guest-bridge image refresh has unpublished input bytes");
            }
        }
        // The input is now canonical guest bytes, including the target's
        // previously GPU-owned partial cells. Existing SynchronizeHost encodes
        // all mips and layers, chains its prior cycle and records versions.
        dirtyState = DirtyState::CpuDirty;
        state.unlock();
        SynchronizeHost(false);
        std::scoped_lock metadata{group->mutex};
        if (regionAuthority.load() != group) continue;
        bool stale{};
        for (const auto &binding : regionBindings)
            stale |= binding.residentVersion != group->regions[binding.region].hostVersion;
        regionStale.store(stale, std::memory_order_relaxed);
        if (!stale) return;
      }
    }
