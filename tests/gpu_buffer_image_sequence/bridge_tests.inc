// SPDX-License-Identifier: MPL-2.0
// Production manager/executor integration: the image and buffer alternate as
// authoritative producers and retain the relationship across each use.
    void CheckMixedSrgbBridge(GPU &gpu, bool eviction) {
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        *gpu.state.settings->freeGuestTextureMemory=eviction;
        auto descriptor=PartialDescriptor(bytes+4096); descriptor.format=format::R8G8B8A8Srgb;
        auto image=gpu.texture.FindOrCreate(descriptor);
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192});
        auto buffer=view.GetBuffer()->shared_from_this();
        operation.Fill(view,0xFFFFFFFF); operation.Submit();
        operation.Read(image.get()); operation.Submit();
        auto encoded=DownloadPartial(image->texture);
        for (size_t i{};i<encoded.size();++i) assert(encoded[i]==(i<4096 ? 0x2D : 0xFF));
        auto group=image->texture->regionAuthority.load();
        assert(group && group==buffer->regionAuthority.load());
        // A failed target staging allocation leaves the writer and relationship
        // intact, and a later retry reconstructs the same bytes.
        operation.Fill(buffer->GetView(0,512),0xABABABAB); operation.Submit();
        const auto serial=buffer->gpuWriteSerial;
        gpu.memory.failAllocation=true; bool rejected{};
        try { operation.Read(image.get()); } catch (const std::bad_alloc &) { rejected=true; }
        gpu.memory.failAllocation=false;
        assert(rejected && group==image->texture->regionAuthority.load() && group==buffer->regionAuthority.load() && buffer->gpuWriteSerial==serial);
        operation.Read(image.get()); operation.Submit(); encoded=DownloadPartial(image->texture);
        for (size_t i{};i<encoded.size();++i) assert(encoded[i]==(i<4096 ? 0x2D : i<4608 ? 0xAB : 0xFF));
        operation.Clear(image.get(),{0,0,0,1}); operation.Submit();
        operation.Read(view); operation.Submit(); buffer->WaitOnFence();
        for (size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4096 ? (i%4==3 ? 0xFF : 0) : 0xFF));
        assert(group==image->texture->regionAuthority.load() && group==buffer->regionAuthority.load());
        operation.Fill(buffer->GetView(16,16),0x78787878); operation.Submit();
        operation.Read(image.get()); operation.Submit(); encoded=DownloadPartial(image->texture);
        for (size_t i{};i<encoded.size();++i) {
            const u8 expected=i<4096 ? (i%4==3 ? 0xFF : 0) : i<4112 || i>=4128 ? (i%4==3 ? 0xFF : 0) : 0x78;
            assert(encoded[i]==expected);
        }
        gpu.state.process->trap.reverseEntries=eviction;
        operation.Clear(image.get(),{1,0,0,1}); operation.Submit();
        assert(gpu.state.process->trap.HandleTrap(bytes+8192,false));
        for (size_t i{};i<4096;++i) assert(bytes[8192+i]==(i%4==0 || i%4==3 ? 0xFF : 0));
        assert(gpu.state.process->trap.HandleTrap(bytes+8192,true));
        // After the CPU invalidation, the buffer becomes the foreign producer
        // observed through both registered image/buffer fault callbacks.
        operation.Fill(buffer->GetView(0,4096),0x42424242); operation.Submit();
        assert(gpu.state.process->trap.HandleTrap(bytes+8192,false));
        for (size_t i{};i<4096;++i) assert(bytes[8192+i]==0x42);
        assert(gpu.state.process->trap.HandleTrap(bytes+8192,true));
        std::fill(bytes+8192,bytes+12288,0x36);
        operation.Read(image.get()); operation.Submit(); encoded=DownloadPartial(image->texture);
        for (size_t i{4096};i<8192;++i) assert(encoded[i]==0x36);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS: persistent sRGB guest bridge, alternating producers, CPU callbacks and allocation retry; FreeGuest=%s\n",eviction?"ON":"OFF");
    }

    void CheckMixedBridgeJoinsPending(GPU &gpu) {
        constexpr size_t size{8*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        auto a=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096));
        operation.Read(a.get()); operation.Clear(a.get(),{0,1,0,1}); operation.Submit();
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=view.GetBuffer()->shared_from_this();
        operation.Read(view); operation.Fill(buffer->GetView(4096,4096),0xB4B4B4B4); operation.Submit();
        auto old=buffer->regionAuthority.load(); assert(old && !old->guestBridge);
        auto readback=gpu.memory.AllocateStagingBuffer(8192);
        operation.Run([&] { operation.executor.AttachBuffer(view); operation.executor.AddOutsideRpCommand([view,readback](auto &cb,const auto &,auto &) {
            cb.copyBuffer(view.GetBuffer()->GetBacking(),readback->vkBuffer,vk::BufferCopy{.size=8192});
            cb.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eTransfer,{},
                vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eTransferRead,.dstAccessMask=vk::AccessFlagBits::eTransferWrite},{},{});
        }); });
        // The later buffer writer is also still pending when registration
        // retries; the queued reader must retain the previous contents.
        operation.Fill(buffer->GetView(4096,4096),0xC6C6C6C6);
        auto descriptor=PartialDescriptor(bytes+3*4096); descriptor.format=format::R8G8B8A8Srgb;
        std::shared_ptr<TextureView> b;
        operation.Run([&] { b=gpu.texture.FindOrCreate(descriptor,operation.executor.tag); });
        operation.Read(b.get()); operation.Submit();
        auto group=buffer->regionAuthority.load(); assert(group && group!=old && group->guestBridge && group==a->texture->regionAuthority.load() && group==b->texture->regionAuthority.load());
        const std::array<u8,4> green{0,255,0,255};
        for (size_t i{};i<8192;++i) assert(readback->data()[i]==(i<4096 ? green[i%4] : 0xB4));
        auto encoded=DownloadPartial(b->texture);
        for (size_t i{};i<8192;++i) assert(encoded[i]==(i<4096 ? 0xC6 : 0x2D));
        operation.Clear(b.get(),{0,0,1,1}); operation.Submit(); operation.Read(view); operation.Submit(); buffer->WaitOnFence();
        for (size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4096 ? green[i%4] : (i%4>=2 ? 0xFF : 0)));
        operation.Fill(buffer->GetView(16,16),0x77777777); operation.Submit(); operation.Read(a.get()); operation.Submit();
        encoded=DownloadPartial(a->texture);
        for (size_t i{};i<8192;++i) assert(encoded[i]==(i>=4112 && i<4128 ? 0x77 : green[i%4]));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: direct component transitions to sRGB guest bridge with pending consumer and retained views");
    }

    // Direct registration is intentional: manager-driven uses discover peers
    // before an overlapping untracked image can become a producer.
    void CheckMixedBridgeNewerNewcomer(GPU &gpu) {
        constexpr size_t size{8*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        auto a=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096));
        operation.Read(a.get()); operation.Clear(a.get(),{1,0,0,1}); operation.Submit();
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=view.GetBuffer()->shared_from_this();
        operation.Read(view); operation.Fill(buffer->GetView(4096,4096),0xB4B4B4B4); operation.Submit();
        auto old=buffer->regionAuthority.load(); assert(old && !old->guestBridge);
        auto descriptor=PartialDescriptor(bytes+3*4096); descriptor.format=format::R8G8B8A8Srgb;
        auto newcomer=std::make_shared<Texture>(gpu,descriptor);
        newcomer->SetupGuestMappings(); newcomer->SynchronizeHost(false);
        newcomer->RecordGpuWrite(); newcomer->SynchronizeHost(true);
        if (newcomer->cycle) newcomer->cycle->WaitSubmit();
        auto previous=newcomer->cycle;
        newcomer->cycle=gpu.scheduler.Submit([&](auto &cb) {
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
            cb.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},
                vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=newcomer->layout,.newLayout=newcomer->layout,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                    .image=newcomer->GetBacking(),.subresourceRange=range});
            cb.clearColorImage(newcomer->GetBacking(),newcomer->layout,vk::ClearColorValue{std::array<float,4>{0,0,1,1}},range);
        });
        newcomer->cycle->AttachObject(newcomer); newcomer->cycle->ChainCycle(previous);
        assert(!newcomer->regionAuthority.load() && newcomer->gpuWriteSerial>buffer->gpuWriteSerial);
        assert(Texture::RegisterMixedRegionAuthority({newcomer},{buffer},{}));
        auto group=buffer->regionAuthority.load();
        assert(group && group!=old && group->guestBridge && group==newcomer->regionAuthority.load());
        operation.Read(view); operation.Submit(); buffer->WaitOnFence();
        for (size_t i{};i<8192;++i) {
            const u8 expected=i<4096 ? (i%4==0 || i%4==3 ? 0xFF : 0) : (i%4>=2 ? 0xFF : 0);
            assert(buffer->backing->data()[i]==expected);
        }
        operation.Read(a.get()); operation.Submit();
        const auto encoded=DownloadPartial(a->texture);
        for (size_t i{};i<8192;++i) assert(encoded[i]==(i%4==0 || i%4==3 ? 0xFF : 0));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: newer untracked GPU image producer replaces inherited overlap and preserves old exclusive bytes");
    }


    void CheckMixedBridgeBudgetRecovery(GPU &gpu) {
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        auto descriptor=PartialDescriptor(bytes+4096); descriptor.format=format::R8G8B8A8Srgb;
        auto image=gpu.texture.FindOrCreate(descriptor);
        operation.Read(image.get()); operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        auto retained=gpu.buffer.FindOrCreate({bytes+8192,8192});
        auto old=retained.GetBuffer()->shared_from_this();
        operation.Read(retained); operation.Fill(old->GetView(4096,4096),0xB4B4B4B4); operation.Submit();
        old->WaitOnFence();
        auto group=old->regionAuthority.load(); assert(group && group->guestBridge);
        // Deterministically create valid fragmentation pressure: split only the
        // buffer-exclusive final cell, preserving its writer and every version.
        // No Vulkan allocation or 4096 repeated coalesces is needed to hit the
        // real production budget. Image bindings keep their original indices.
        const auto compactRegions=group->regions;
        const auto compactBindings=old->regionBindings;
        {
            std::scoped_lock resource{*old}; std::scoped_lock state{old->stateMutex}; std::scoped_lock metadata{group->mutex};
            const auto tail=group->regions.back();
            assert(tail.begin==reinterpret_cast<uintptr_t>(bytes+12288) && tail.end==reinterpret_cast<uintptr_t>(bytes+16384));
            assert(tail.bufferWriterIdentity==old.get());
            const auto binding=old->regionBindings.back();
            assert(binding.region==group->regions.size()-1);
            group->regions.pop_back(); old->regionBindings.pop_back();
            auto cursor=tail.begin;
            while(group->regions.size()<texture::GuestBridgeMaxRegions) {
                auto cell=tail; cell.begin=cursor;
                cell.end=group->regions.size()+1==texture::GuestBridgeMaxRegions ? tail.end : cursor+1;
                assert(cell.begin<cell.end);
                old->regionBindings.push_back({group->regions.size(),cell.begin-reinterpret_cast<uintptr_t>(bytes+8192),binding.residentVersion});
                group->regions.push_back(cell); cursor=cell.end;
            }
            assert(cursor==tail.end);
        }
        auto consumer=gpu.memory.AllocateStagingBuffer(8192);
        operation.Run([&] { operation.executor.AttachBuffer(retained); operation.executor.AddOutsideRpCommand([old,consumer](auto &cb,const auto &,auto &) {
            cb.copyBuffer(old->GetBacking(),consumer->vkBuffer,vk::BufferCopy{.size=8192});
        }); });
        const auto regions=group->regions; const auto bindings=old->regionBindings;
        const std::vector<u8> guestBefore(bytes,bytes+size);
        const auto serial=old->gpuWriteSerial; const auto nodes=operation.executor.slot->nodes.size();
        bool rejected{};
        try { operation.Run([&] { (void)gpu.buffer.FindOrCreate({bytes,4*4096},operation.executor.tag,[&](auto buffer,auto &&lock) {
            operation.executor.AttachLockedBuffer(std::move(buffer),std::move(lock));
        }); }); } catch(const std::runtime_error &error) {
            rejected=std::string_view(error.what()).find("region budget exceeded during coalesce")!=std::string_view::npos;
        }
        assert(rejected && group==old->regionAuthority.load() && group==image->texture->regionAuthority.load());
        assert(retained.GetBuffer()==old.get() && retained.GetOffset()==0 && old->gpuWriteSerial==serial);
        assert(operation.executor.slot->nodes.size()==nodes && group->regions.size()==regions.size() && old->regionBindings.size()==bindings.size());
        assert(std::equal(guestBefore.begin(),guestBefore.end(),bytes));
        for(size_t i{};i<regions.size();++i) {
            const auto &a=regions[i], &b=group->regions[i];
            assert(a.begin==b.begin && a.end==b.end && a.hostVersion==b.hostVersion && a.guestVersion==b.guestVersion &&
                a.writerIdentity==b.writerIdentity && a.bufferWriterIdentity==b.bufferWriterIdentity);
        }
        for(size_t i{};i<bindings.size();++i) assert(bindings[i].region==old->regionBindings[i].region &&
            bindings[i].offset==old->regionBindings[i].offset && bindings[i].residentVersion==old->regionBindings[i].residentVersion);
        // Release only the synthetic pressure by restoring the equivalent
        // compact partition. This is a test fixture operation, not a claimed
        // production compactor. Retry the same manager operation and consumer.
        {
            std::scoped_lock metadata{group->mutex}; group->regions=compactRegions; old->regionBindings=compactBindings;
        }
        BufferView joined;
        operation.Run([&] { joined=gpu.buffer.FindOrCreate({bytes,4*4096},operation.executor.tag,[&](auto buffer,auto &&lock) {
            operation.executor.AttachLockedBuffer(std::move(buffer),std::move(lock));
        }); });
        operation.Read(joined); operation.Submit(); joined.GetBuffer()->WaitOnFence();
        assert(retained.GetBuffer()==joined.GetBuffer() && retained.GetOffset()==8192);
        const std::array<u8,4> green{0,255,0,255};
        for(size_t i{};i<8192;++i) assert(consumer->data()[i]==(i<4096 ? green[i%4] : 0xB4));
        for(size_t i{};i<joined.size;++i) assert(joined.GetBuffer()->backing->data()[i]==
            (i<4096 ? 0x2D : i<12288 ? green[i%4] : 0xB4));
        operation.Fill(retained,0x5E5E5E5E); operation.Submit(); operation.Read(image.get()); operation.Submit();
        const auto encoded=DownloadPartial(image->texture);
        for(size_t i{};i<8192;++i) assert(encoded[i]==(i<4096 ? green[i%4] : 0x5E));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: bridge metadata exhaustion preserves bytes/versions/group/delegates/pending consumer; same-operation retry recovers after pressure removal");
    }
