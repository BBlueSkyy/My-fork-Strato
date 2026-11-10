// SPDX-License-Identifier: MPL-2.0
// Shared partition registration; included in texture.cpp after region_authority.inc.
    bool Texture::RegisterMixedRegionAuthority(std::vector<std::shared_ptr<Texture>> images,
                                               std::vector<std::shared_ptr<Buffer>> buffers, ContextTag context) {
        std::vector<std::shared_ptr<texture::RegionAuthority>> previous;
        auto expand = [&](const auto &resource) {
            if (auto group{resource->regionAuthority.load()}; group && std::find(previous.begin(), previous.end(), group) == previous.end()) {
                previous.push_back(group);
                std::scoped_lock metadata{group->mutex};
                for (const auto &weak : group->members) if (auto member{weak.lock()}; member && std::find(images.begin(), images.end(), member) == images.end()) images.push_back(member);
                for (const auto &weak : group->buffers) if (auto member{weak.lock()}; member && std::find(buffers.begin(), buffers.end(), member) == buffers.end()) buffers.push_back(member);
            }
        };
        for (size_t i{}, j{}; i < images.size() || j < buffers.size();) {
            if (i < images.size()) { auto member{images[i++]}; expand(member); }
            if (j < buffers.size()) { auto member{buffers[j++]}; expand(member); }
        }
        if (images.empty() || buffers.empty()) return false;
        // Host format is fixed at Texture construction. Existing group mode is
        // immutable after publication, so this early check needs no state lock.
        bool guestBridge{};
        for (const auto &old : previous) guestBridge |= old->guestBridge;
        for (const auto &image : images)
            guestBridge |= image->format && image->format->vkFormat == vk::Format::eR8G8B8A8Srgb;
        if (guestBridge && (images.size() > 128 || buffers.size() > 128)) return false;
        if (context && (std::any_of(images.begin(), images.end(), [&](const auto &image) { return image->tag.load() == context; }) ||
            std::any_of(buffers.begin(), buffers.end(), [&](const auto &buffer) { return buffer->tag.load() == context; }))) {
            TextureAliasReconciliationRequest request;
            request.mixedRegistrationImages = images; request.mixedRegistrationBuffers = buffers;
            throw request;
        }
        std::sort(buffers.begin(), buffers.end(), [](const auto &a, const auto &b) { return std::less<Buffer *>{}(a.get(), b.get()); });
        std::sort(images.begin(), images.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        std::vector<ContextLock<Buffer>> bufferLocks;
        std::vector<ContextLock<Texture>> imageLocks;
        for (const auto &buffer : buffers) bufferLocks.emplace_back(context, *buffer);
        for (const auto &image : images) imageLocks.emplace_back(context, *image);
        std::vector<std::unique_lock<RecursiveSpinLock>> bufferStates;
        std::vector<std::unique_lock<std::recursive_mutex>> imageStates;
        for (const auto &buffer : buffers) bufferStates.emplace_back(buffer->stateMutex);
        for (const auto &image : images) imageStates.emplace_back(image->stateMutex);
        std::vector<std::vector<texture::PhysicalImageSpan>> spans;
        std::vector<uintptr_t> endpoints;
        auto appendEndpoints = [&](uintptr_t begin, uintptr_t end) {
            if (guestBridge && endpoints.size() > texture::GuestBridgeMaxEndpoints - 2) return false;
            endpoints.push_back(begin); endpoints.push_back(end);
            return true;
        };
        for (const auto &image : images) {
            if (!image->guest || image->aliasGroup.load() || image->aliasReadOnly.load() || image->aliasRetired.load() ||
                image->format != image->guest->format || !IsGuestLayoutReversible(*image->guest) ||
                (image->format->vkFormat != vk::Format::eR8G8B8A8Unorm && image->format->vkFormat != vk::Format::eR8G8B8A8Srgb) ||
                image->guest->mipLevelCount != 1 || image->guest->GetBackingLayerCount() != 1 || image->guest->dimensions.depth != 1 ||
                image->tiling != vk::ImageTiling::eOptimal || image->sampleCount != vk::SampleCountFlagBits::e1) return false;
            // The first bridge increment admits only encoder-proven complete
            // allocations; padding and stride gaps remain explicit boundaries.
            if (guestBridge && !CanDiscardGuest(*image->guest, image->mirror.size())) return false;
            spans.push_back(PhysicalImageSpans(*image->guest, true));
            if (spans.back().empty() || spans.back().size() > 128) return false;
            for (const auto &span : spans.back()) {
                if (span.end - span.begin != 512) return false;
                if (!appendEndpoints(span.begin, span.end)) return false;
            }
        }
        for (const auto &buffer : buffers) {
            if (!buffer->guest || buffer->isDirect) return false;
            const auto begin{reinterpret_cast<uintptr_t>(buffer->guest->data())};
            if (buffer->guest->size() > UINTPTR_MAX - begin || !appendEndpoints(begin, begin + buffer->guest->size())) return false;
        }
        std::vector<std::unique_lock<std::recursive_mutex>> metadata;
        std::sort(previous.begin(), previous.end(), [](const auto &a, const auto &b) { return std::less<texture::RegionAuthority *>{}(a.get(), b.get()); });
        for (const auto &group : previous) {
            metadata.emplace_back(group->mutex);
            for (const auto &region : group->regions)
                if (!appendEndpoints(region.begin, region.end)) return false;
        }
        std::sort(endpoints.begin(), endpoints.end()); endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        auto group{std::make_shared<texture::RegionAuthority>()};
        group->guestBridge = guestBridge;
        std::vector<std::vector<texture::PhysicalImageBinding>> imageBindings(images.size());
        std::vector<std::vector<texture::PhysicalBufferBinding>> bufferBindings(buffers.size());
        for (size_t i{1}; i < endpoints.size(); ++i) {
            const auto begin{endpoints[i - 1]}, end{endpoints[i]};
            bool covered{};
            for (size_t j{}; j < images.size(); ++j) for (const auto &span : spans[j]) if (span.begin <= begin && span.end >= end) {
                if (begin != span.begin || end != span.end) return false;
                imageBindings[j].push_back({group->regions.size(), span.offset, span.level, span.layer, span.y, span.rowBytes, span.rowHeight, 0, span.x, span.z, span.width});
                covered = true; break;
            }
            for (size_t j{}; j < buffers.size(); ++j) {
                const auto base{reinterpret_cast<uintptr_t>(buffers[j]->guest->data())};
                if (base <= begin && base + buffers[j]->guest->size() >= end) {
                    bufferBindings[j].push_back({group->regions.size(), begin - base, 0}); covered = true;
                }
            }
            if (!covered) continue;
            if (guestBridge && group->regions.size() >= texture::GuestBridgeMaxRegions) return false;
            texture::PhysicalImageRegion region{begin, end};
            bool inherited{};
            for (const auto &old : previous) for (const auto &source : old->regions) if (source.begin <= begin && source.end >= end) {
                region = source; region.begin = begin; region.end = end; inherited = true; break;
            }
            {
                // Untracked newcomers may have a newer producer than an inherited cell.
                for (size_t j{}; j < images.size(); ++j) if (!imageBindings[j].empty() && imageBindings[j].back().region == group->regions.size()) {
                    const auto &image{images[j]};
                    if ((!inherited || (guestBridge && !image->regionAuthority.load())) && image->gpuWriteSerial > region.hostVersion && image->gpuWriteSerial > image->gpuReadbackSerial) {
                        region.writerIdentity = image.get(); region.writer = image;
                        region.bufferWriterIdentity = {}; region.bufferWriter.reset(); region.hostVersion = image->gpuWriteSerial;
                    }
                }
                for (size_t j{}; j < buffers.size(); ++j) if (!bufferBindings[j].empty() && bufferBindings[j].back().region == group->regions.size()) {
                    const auto &buffer{buffers[j]};
                    if ((!inherited || (guestBridge && !buffer->regionAuthority.load())) && buffer->dirtyState == Buffer::DirtyState::GpuDirty && buffer->gpuWriteSerial > region.hostVersion) {
                        region.writerIdentity = {}; region.writer.reset(); region.bufferWriterIdentity = buffer.get();
                        region.bufferWriter = buffer; region.hostVersion = buffer->gpuWriteSerial;
                    }
                }
            }
            group->regions.push_back(region);
        }
        auto resident = [&](const auto &resource, auto &binding) {
            const auto &region{group->regions[binding.region]};
            binding.residentVersion = region.guestVersion;
            if (auto old{resource->regionAuthority.load()}) for (const auto &source : resource->regionBindings) {
                const auto &prior{old->regions[source.region]};
                if (prior.begin <= region.begin && prior.end >= region.end) { binding.residentVersion = source.residentVersion; return; }
            }
        };
        for (size_t j{}; j < images.size(); ++j) for (auto &binding : imageBindings[j]) {
            resident(images[j], binding); auto &region{group->regions[binding.region]};
            if (!images[j]->regionAuthority.load() && region.writerIdentity == images[j].get()) binding.residentVersion = region.hostVersion;
        }
        for (size_t j{}; j < buffers.size(); ++j) for (auto &binding : bufferBindings[j]) {
            resident(buffers[j], binding); auto &region{group->regions[binding.region]};
            if (!buffers[j]->regionAuthority.load() && region.bufferWriterIdentity == buffers[j].get()) binding.residentVersion = region.hostVersion;
        }
        for (const auto &image : images) group->members.push_back(image);
        for (const auto &buffer : buffers) group->buffers.push_back(buffer);
        std::vector<std::vector<std::weak_ptr<Texture>>> neighbors(images.size());
        for (size_t i{}; i < images.size(); ++i) for (size_t j{}; j < images.size(); ++j) if (i != j) neighbors[i].push_back(images[j]);
        for (size_t j{}; j < images.size(); ++j) {
            images[j]->regionBindings = std::move(imageBindings[j]); images[j]->regionNeighbors = std::move(neighbors[j]);
            images[j]->regionAuthority.store(group); images[j]->regionMixed = true;
            images[j]->regionTracked = true; images[j]->regionStale = true;
            images[j]->bufferLookupRevision = images[j]->gpu.buffer.regionRevision;
        }
        for (size_t j{}; j < buffers.size(); ++j) {
            buffers[j]->regionBindings = std::move(bufferBindings[j]); buffers[j]->regionAuthority.store(group);
            buffers[j]->regionTracked = true; buffers[j]->regionStale = true;
            buffers[j]->regionLookupRevision = buffers[j]->gpu.texture.RegionRevision();
        }
        ++texture::regionInvalidationSequence;
        return true;
    }
