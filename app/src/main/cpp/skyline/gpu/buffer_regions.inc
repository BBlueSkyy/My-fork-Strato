// SPDX-License-Identifier: MPL-2.0
// Included by buffer.cpp, inside skyline::gpu.
    void Buffer::RequireRegionCoherence(ContextTag context) {
        if constexpr (texture::MixedRegionAuthorityEnabled)
            if (regionLookupRevision != gpu.texture.RegionRevision())
                gpu.texture.RegisterBufferRegions(shared_from_this(), context);
        if (regionStale.load(std::memory_order_relaxed)) {
            TextureAliasReconciliationRequest request; request.bufferTarget = shared_from_this(); throw request;
        }
    }

    void Buffer::PrepareGpuWrite(UsageTracker &usageTracker) {
        if (guest) usageTracker.dirtyIntervals.Insert(*guest);
        SynchronizeHost();
        if (guest && !isDirect) gpu.state.process->trap.TrapRegions(*trapHandle, false);
        BlockAllCpuBackingWrites();
    }

    void Buffer::InvalidateRegionPeers() {
        if (auto group{regionAuthority.load()}) {
            for (const auto &weak : group->members) if (auto image{weak.lock()}; image && !image->regionStale.exchange(true, std::memory_order_relaxed)) ++texture::regionInvalidationSequence;
            for (const auto &weak : group->buffers) if (auto buffer{weak.lock()}; buffer && buffer.get() != this && !buffer->regionStale.exchange(true, std::memory_order_relaxed)) ++texture::regionInvalidationSequence;
        }
    }

    void Buffer::RecordRegionWrite(vk::DeviceSize offset, vk::DeviceSize size) {
        auto group{regionAuthority.load()}; if (!group) return;
        std::scoped_lock metadata{group->mutex};
        const auto first{reinterpret_cast<uintptr_t>(guest->data()) + offset};
        const auto last{first + size};
        for (auto &binding : regionBindings) {
            auto &region{group->regions[binding.region]};
            if (region.begin >= last || region.end <= first) continue;
            region.writerIdentity = {}; region.writer.reset(); region.bufferWriterIdentity = this; region.bufferWriter = weak_from_this();
            region.hostVersion = gpuWriteSerial; binding.residentVersion = gpuWriteSerial;
        }
        InvalidateRegionPeers();
    }

    void Buffer::CommitGpuWrite(UsageTracker &, vk::DeviceSize offset, vk::DeviceSize size) {
        ++writeGeneration; gpuWriteSerial = ++texture::gpuWriteSequence;
        currentExecutionGpuDirty = true;
        if (guest) {
            if (isDirect) directGpuWritesActive = true;
            else dirtyState = DirtyState::GpuDirty;
        }
        AdvanceSequence();
        RecordRegionWrite(offset, size);
    }

    void Buffer::ValidateRegionWrites(span<BufferView> buffers, span<TextureView *> images) {
        struct Write { Buffer *buffer; uintptr_t first, last; };
        boost::container::small_vector<Write, 8> writes;
        for (const auto &view : buffers) {
            auto buffer{view.GetBuffer()};
            if (!buffer->regionTracked.load(std::memory_order_relaxed)) continue;
            const auto first{reinterpret_cast<uintptr_t>(buffer->guest->data()) + view.GetOffset()};
            writes.push_back({buffer, first, first + view.size});
        }
        for (const auto &write : writes) {
            auto group{write.buffer->regionAuthority.load()}; if (!group) continue;
            std::scoped_lock metadata{group->mutex};
            for (const auto &binding : write.buffer->regionBindings) {
                const auto &region{group->regions[binding.region]};
                if (region.begin >= write.last || region.end <= write.first) continue;
                for (const auto &other : writes) {
                    if (other.buffer == write.buffer || other.buffer->regionAuthority.load() != group || other.first >= region.end || other.last <= region.begin) continue;
                    if (std::any_of(other.buffer->regionBindings.begin(), other.buffer->regionBindings.end(), [&](const auto &entry) { return entry.region == binding.region; }))
                        throw exception("Simultaneous distinct buffer physical region writers");
                }
                for (const auto image : images) if (image && image->texture->regionAuthority.load() == group)
                    for (const auto &imageBinding : image->texture->regionBindings) if (imageBinding.region == binding.region)
                        throw exception("Simultaneous mixed physical region writers");
            }
        }
    }

    void Buffer::ReleaseRegionHost() {
        auto group{regionAuthority.load()}; if (!group) return;
        for (const auto &binding : regionBindings) {
            auto &region{group->regions[binding.region]};
            if (group->transport == texture::RegionTransport::GuestBridge ||
                !region.HasWriter() || region.bufferWriterIdentity == this) {
                region.bufferWriterIdentity = {}; region.bufferWriter.reset(); region.writerIdentity = {}; region.writer.reset();
                region.hostVersion = region.guestVersion = ++texture::gpuWriteSequence;
            }
        }
        regionStale = true; InvalidateRegionPeers(); ++texture::regionInvalidationSequence;
    }

    bool Buffer::NeedsRegionGuest() const {
        auto group{regionAuthority.load()}; if (!group) return false;
        std::scoped_lock metadata{group->mutex};
        for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (region.hostVersion != region.guestVersion) return true;
        }
        return false;
    }

    void Buffer::PublishRegionGuest() {
      for (;;) {
        auto group{regionAuthority.load()}; if (!group) return;
        std::vector<std::shared_ptr<Buffer>> buffers{shared_from_this()};
        std::vector<std::shared_ptr<Texture>> images;
        {
            std::scoped_lock metadata{group->mutex};
            if (regionAuthority.load() != group) continue;
            // A component can connect resources outside this buffer. Only
            // unpublished owners of its actual physical cells are dependencies.
            for (const auto &binding : regionBindings) {
                const auto &region{group->regions[binding.region]};
                if (region.hostVersion == region.guestVersion) continue;
                if (auto source{region.bufferWriter.lock()}; source && std::find(buffers.begin(), buffers.end(), source) == buffers.end()) buffers.push_back(source);
                if (auto source{region.writer.lock()}; source && std::find(images.begin(), images.end(), source) == images.end()) images.push_back(source);
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
                const auto &region{group->regions[binding.region]};
                if (region.hostVersion == region.guestVersion) continue;
                if (region.bufferWriterIdentity && std::none_of(buffers.begin(), buffers.end(), [&](const auto &source) { return source.get() == region.bufferWriterIdentity; })) valid = false;
                if (region.writerIdentity && std::none_of(images.begin(), images.end(), [&](const auto &source) { return source.get() == region.writerIdentity; })) valid = false;
            }
            if (!valid) continue;
        }
        // No metadata is held while waiting for a necessary producer or
        // publishing its independently masked cells. A later writer causes the
        // nonblocking trap/read validation to prepare another snapshot.
        for (const auto &buffer : buffers) buffer->SynchronizeGuest();
        for (const auto &image : images) image->SynchronizeGuest(false, true);
        return;
      }
    }


    void Buffer::ReconcileGuestBridgeRegions() {
      for (;;) {
        auto group{regionAuthority.load()};
        if (!group || group->transport != texture::RegionTransport::GuestBridge ||
            !regionStale.load(std::memory_order_relaxed)) return;
        // This existing path publishes precisely the target buffer's newest
        // foreign buffer/image donors through their region-owned readbacks.
        PublishRegionGuest();
        std::unique_lock resource{*this};
        std::unique_lock state{stateMutex};
        if (regionAuthority.load() != group) continue;
        {
            std::scoped_lock metadata{group->mutex};
            for (const auto &binding : regionBindings) {
                const auto &cell{group->regions[binding.region]};
                if (cell.hostVersion != cell.guestVersion)
                    throw exception("Guest-bridge buffer refresh has unpublished input bytes");
            }
        }
        // WaitOnFence in SynchronizeHost protects pending GPU readers before
        // copying the full canonical guest backing over this staged buffer.
        dirtyState = DirtyState::CpuDirty;
        state.unlock();
        SynchronizeHost();
        std::scoped_lock metadata{group->mutex};
        if (regionAuthority.load() != group) continue;
        bool stale{};
        for (const auto &binding : regionBindings)
            stale |= binding.residentVersion != group->regions[binding.region].hostVersion;
        regionStale.store(stale, std::memory_order_relaxed);
        if (!stale) return;
      }
    }

    void Buffer::ReconcileRegions() {
        gpu.texture.RegisterBufferRegions(shared_from_this(), {});
        if (auto group{regionAuthority.load()}; group && group->transport == texture::RegionTransport::GuestBridge) {
            ReconcileGuestBridgeRegions();
            return;
        }
      for (;;) {
        auto group{regionAuthority.load()}; if (!group || !regionStale.load()) return;
        std::vector<std::shared_ptr<Buffer>> buffers{shared_from_this()};
        std::vector<std::shared_ptr<Texture>> images;
        {
            std::scoped_lock metadata{group->mutex};
            for (const auto &binding : regionBindings) {
                const auto &region{group->regions[binding.region]};
                if (binding.residentVersion == region.hostVersion) continue;
                if (auto source{region.bufferWriter.lock()}; source && std::find(buffers.begin(), buffers.end(), source) == buffers.end()) buffers.push_back(source);
                if (auto source{region.writer.lock()}; source && std::find(images.begin(), images.end(), source) == images.end()) images.push_back(source);
            }
        }
        std::sort(buffers.begin(), buffers.end(), [](const auto &a, const auto &b) { return std::less<Buffer *>{}(a.get(), b.get()); });
        std::sort(images.begin(), images.end(), [](const auto &a, const auto &b) { return std::less<Texture *>{}(a.get(), b.get()); });
        std::vector<std::unique_lock<Buffer>> bufferLocks;
        std::vector<std::unique_lock<Texture>> imageLocks;
        for (const auto &buffer : buffers) bufferLocks.emplace_back(*buffer);
        for (const auto &image : images) imageLocks.emplace_back(*image);
        std::unique_lock state{stateMutex};
        if (regionAuthority.load() != group) continue;
        SynchronizeHost();
        std::unique_lock metadata{group->mutex};
        bool donorsValid{regionAuthority.load() == group};
        if (donorsValid) for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]};
            if (binding.residentVersion == region.hostVersion) continue;
            if (region.bufferWriterIdentity && std::none_of(buffers.begin(), buffers.end(), [&](const auto &source) { return source.get() == region.bufferWriterIdentity; })) donorsValid = false;
            if (region.writerIdentity && std::none_of(images.begin(), images.end(), [&](const auto &source) { return source.get() == region.writerIdentity; })) donorsValid = false;
        }
        if (!donorsValid) continue;
        struct ImageTransfer { std::shared_ptr<Texture> source; std::vector<texture::RawRgba8Gob> gobs; std::vector<vk::BufferImageCopy> copies; };
        struct BufferTransfer { std::shared_ptr<Buffer> source; std::vector<vk::BufferCopy> copies; };
        std::vector<ImageTransfer> imageTransfers;
        std::vector<BufferTransfer> bufferTransfers;
        std::vector<uint64_t> versions;
        std::vector<std::pair<size_t, size_t>> guestCopies;
        for (const auto &binding : regionBindings) {
            const auto &region{group->regions[binding.region]}; versions.push_back(region.hostVersion);
            if (binding.residentVersion == region.hostVersion) continue;
            if (!region.HasWriter()) { guestCopies.emplace_back(binding.offset, region.end - region.begin); continue; }
            if (region.bufferWriterIdentity == this) throw exception("Buffer owner lost its resident version");
            if (auto source{region.writer.lock()}) {
                auto origin{std::find_if(source->regionBindings.begin(), source->regionBindings.end(), [&](const auto &entry) { return entry.region == binding.region; })};
                if (origin == source->regionBindings.end() || region.end - region.begin != 512) throw exception("Invalid mixed image source binding");
                auto transfer{std::find_if(imageTransfers.begin(), imageTransfers.end(), [&](const auto &entry) { return entry.source == source; })};
                if (transfer == imageTransfers.end()) { imageTransfers.push_back({source, {}, {}}); transfer = std::prev(imageTransfers.end()); }
                transfer->gobs.push_back({binding.offset, origin->x, origin->y});
            } else if (auto source{region.bufferWriter.lock()}) {
                auto origin{std::find_if(source->regionBindings.begin(), source->regionBindings.end(), [&](const auto &entry) { return entry.region == binding.region; })};
                if (origin == source->regionBindings.end()) throw exception("Invalid mixed buffer source binding");
                auto transfer{std::find_if(bufferTransfers.begin(), bufferTransfers.end(), [&](const auto &entry) { return entry.source == source; })};
                if (transfer == bufferTransfers.end()) { bufferTransfers.push_back({source, {}}); transfer = std::prev(bufferTransfers.end()); }
                transfer->copies.push_back({origin->offset, binding.offset, region.end - region.begin});
            } else throw exception("Physical region lost its host producer");
        }
        for (auto &transfer : imageTransfers) {
            auto copies{texture::PlanRawRgba8GobCopies(transfer.gobs, backing->size(), transfer.source->dimensions)};
            if (!copies) throw exception("Unproved image to raw buffer transfer");
            transfer.copies = std::move(*copies);
        }
        metadata.unlock();
        if (!guestCopies.empty()) {
            WaitOnFence();
            for (const auto &[offset, size] : guestCopies) std::memcpy(backing->data() + offset, mirror.data() + offset, size);
        }
        if (!imageTransfers.empty() || !bufferTransfers.empty()) {
            if (cycle) cycle->WaitSubmit();
            for (const auto &transfer : imageTransfers) if (transfer.source->cycle) transfer.source->cycle->WaitSubmit();
            for (const auto &transfer : bufferTransfers) if (transfer.source->cycle) transfer.source->cycle->WaitSubmit();
            auto commands{gpu.scheduler.AllocateCommandBuffer()}; auto copyCycle{commands.GetFenceCycle()};
            try {
                copyCycle->AttachObject(shared_from_this()); copyCycle->ChainCycle(cycle);
                for (const auto &transfer : imageTransfers) { copyCycle->AttachObject(transfer.source); copyCycle->ChainCycle(transfer.source->cycle); }
                for (const auto &transfer : bufferTransfers) { copyCycle->AttachObject(transfer.source); copyCycle->ChainCycle(transfer.source->cycle); }
                commands->begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
                vk::BufferMemoryBarrier targetBefore{.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite, .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = GetBacking(), .offset = 0, .size = VK_WHOLE_SIZE};
                commands->pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, targetBefore, {});
                for (const auto &transfer : imageTransfers) {
                    vk::ImageMemoryBarrier before{.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite, .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                        .oldLayout = transfer.source->layout, .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = transfer.source->GetBacking(), .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, before);
                    commands->copyImageToBuffer(transfer.source->GetBacking(), vk::ImageLayout::eTransferSrcOptimal, GetBacking(), transfer.copies);
                    auto after{before}; after.srcAccessMask = vk::AccessFlagBits::eTransferRead; after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                    after.oldLayout = before.newLayout; after.newLayout = before.oldLayout;
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, after);
                }
                for (const auto &transfer : bufferTransfers) {
                    auto sourceBefore{targetBefore}; sourceBefore.buffer = transfer.source->GetBacking(); sourceBefore.dstAccessMask = vk::AccessFlagBits::eTransferRead;
                    commands->pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, sourceBefore, {});
                    commands->copyBuffer(transfer.source->GetBacking(), GetBacking(), transfer.copies);
                }
                auto targetAfter{targetBefore}; targetAfter.srcAccessMask = vk::AccessFlagBits::eTransferWrite; targetAfter.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                commands->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, targetAfter, {});
                commands->end(); gpu.scheduler.SubmitCommandBuffer(*commands, copyCycle);
            } catch (...) { copyCycle->Cancel(); throw; }
            cycle = copyCycle;
            for (const auto &transfer : imageTransfers) transfer.source->cycle = copyCycle;
            for (const auto &transfer : bufferTransfers) transfer.source->cycle = copyCycle;
        }
        metadata.lock();
        bool stale{}; size_t i{};
        for (auto &binding : regionBindings) { binding.residentVersion = versions[i++]; stale |= binding.residentVersion != group->regions[binding.region].hostVersion; }
        regionStale = stale; AdvanceSequence();
        if (!stale) return;
      }
    }

    void Buffer::MigrateRegionAuthority(const std::shared_ptr<Buffer> &target, span<std::shared_ptr<Buffer>> sources) {
        std::shared_ptr<texture::RegionAuthority> group;
        for (const auto &source : sources) if (auto old{source->regionAuthority.load()}) {
            if (group && group != old) throw exception("Coalescing distinct mixed authority components is unsupported");
            group = old;
        }
        if (!group) return;
        std::scoped_lock metadata{group->mutex};
        const auto first{reinterpret_cast<uintptr_t>(target->guest->data())}, last{first + target->guest->size()};
        auto regions{group->regions};
        std::vector<texture::PhysicalBufferBinding> bindings;
        std::vector<std::pair<uintptr_t, uintptr_t>> covered;
        bool stale{}, owns{};
        for (size_t i{}; i < regions.size(); ++i) {
            auto &region{regions[i]};
            if (region.begin >= last || region.end <= first) continue;
            if (region.begin < first || region.end > last) throw exception("Coalesce cuts mixed authority cell");
            texture::PhysicalBufferBinding binding{i, region.begin - first, region.guestVersion};
            for (const auto &source : sources) if (source->regionAuthority.load() == group) {
                for (const auto &old : source->regionBindings) if (old.region == i) binding.residentVersion = old.residentVersion;
                if (region.bufferWriterIdentity == source.get()) { region.bufferWriterIdentity = target.get(); region.bufferWriter = target; }
            }
            owns |= region.bufferWriterIdentity == target.get() && region.hostVersion != region.guestVersion;
            stale |= binding.residentVersion != region.hostVersion;
            bindings.push_back(binding); covered.emplace_back(region.begin, region.end);
        }
        std::sort(covered.begin(), covered.end());
        auto addGuest = [&](uintptr_t begin, uintptr_t end) {
            if (begin == end) return;
            bindings.push_back({regions.size(), begin - first, 0}); regions.push_back({begin, end});
        };
        auto cursor{first};
        for (const auto &[begin, end] : covered) { addGuest(cursor, begin); cursor = end; }
        addGuest(cursor, last);
        auto members{group->buffers};
        members.erase(std::remove_if(members.begin(), members.end(), [&](const auto &weak) {
            auto buffer{weak.lock()}; return !buffer || std::find(sources.begin(), sources.end(), buffer) != sources.end();
        }), members.end());
        members.push_back(target);
        // All allocations precede identity replacement; raw source bytes have
        // already been copied and delegates still identify the old resources.
        group->regions = std::move(regions); group->buffers = std::move(members);
        target->regionBindings = std::move(bindings); target->regionAuthority.store(group); target->regionTracked = true; target->regionStale = stale;
        if (owns) target->dirtyState = DirtyState::GpuDirty;
        for (const auto &source : sources) if (source->regionAuthority.load() == group) {
            source->regionAuthority.store({}); source->regionTracked = false; source->regionStale = false; source->regionBindings.clear();
        }
    }
