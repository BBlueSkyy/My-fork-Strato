// SPDX-License-Identifier: MPL-2.0
// A focused release-blocker proof, NOT a selective-admission implementation.
// Automatic discovery stays off, matching a component hypothetically withdrawn
// to the legacy path. Registration, drain, writer claims and transport below
// are the actual production bodies used by the real-Vulkan fixture.
    void CheckMixedWithdrawalBoundary(GPU &gpu, bool withdraw, bool eviction) {
        assert(!texture::MixedRegionAuthorityEnabled);
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        *gpu.state.settings->freeGuestTextureMemory=eviction;
        auto descriptor=PartialDescriptor(bytes+4096); descriptor.format=format::R8G8B8A8Srgb;
        auto image=gpu.texture.FindOrCreate(descriptor);
        auto retained=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=retained.GetBuffer()->shared_from_this();
        assert(Texture::RegisterMixedRegionAuthority({image->texture},{buffer},{}));
        operation.Read(image.get()); operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        operation.Read(retained); operation.Submit(); buffer->WaitOnFence();
        auto group=buffer->regionAuthority.load(); assert(group && group->guestBridge);
        const std::array<u8,4> green{0,255,0,255};
        for(size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4096 ? green[i%4] : 0x2D));
        // Pitched RGBA8 is an existing materializable image path, outside the
        // proven reversible-authority domain. Do not broaden admission here.
        auto peerDescriptor=PartialDescriptor(bytes+8192);
        peerDescriptor.tileConfig={.mode=texture::TileMode::Pitch,.pitch=256};
        auto peer=std::make_shared<Texture>(gpu,peerDescriptor); peer->SetupGuestMappings();
        assert(Texture::CanMaterializeGuestLayout(peerDescriptor));
        assert(Texture::PhysicalImageSpans(peerDescriptor,true).empty());
        auto consumer=gpu.memory.AllocateStagingBuffer(8192);
        operation.Run([&] { operation.executor.AttachBuffer(retained); operation.executor.AddOutsideRpCommand([buffer,consumer](auto &cb,const auto &,auto &) {
            cb.copyBuffer(buffer->GetBacking(),consumer->vkBuffer,vk::BufferCopy{.size=8192});
        }); });
        const auto queued=operation.executor.slot->nodes.size();
        const auto regions=group->regions; const auto bindings=buffer->regionBindings;
        const auto writer=buffer->gpuWriteSerial;
        bool deferred{};
        try { (void)Texture::RegisterMixedRegionAuthority({peer},{buffer},operation.executor.tag); }
        catch(const TextureAliasReconciliationRequest &request) {
            deferred=!request.mixedRegistrationImages.empty() && !request.mixedRegistrationBuffers.empty();
        }
        assert(deferred && operation.executor.slot->nodes.size()==queued && buffer->gpuWriteSerial==writer);
        assert(group==buffer->regionAuthority.load() && group==image->texture->regionAuthority.load() && !peer->regionAuthority.load());
        for(size_t i{};i<regions.size();++i) assert(regions[i].hostVersion==group->regions[i].hostVersion && regions[i].guestVersion==group->regions[i].guestVersion);
        for(size_t i{};i<bindings.size();++i) assert(bindings[i].residentVersion==buffer->regionBindings[i].residentVersion);
        operation.Submit(); buffer->WaitOnFence();
        for(size_t i{};i<8192;++i) assert(consumer->data()[i]==(i<4096 ? green[i%4] : 0x2D));
        assert(!Texture::RegisterMixedRegionAuthority({peer},{buffer},{}));
        assert(!peer->regionAuthority.load() && group==buffer->regionAuthority.load() && group==image->texture->regionAuthority.load());
        if(withdraw) {
            Texture::DrainRegionAuthority({image->texture},{});
            assert(!buffer->regionTracked && !image->texture->regionTracked && !buffer->regionAuthority.load() && !image->texture->regionAuthority.load());
            for(size_t i{};i<size;++i) assert(bytes[i]==(i>=4096 && i<12288 ? green[i%4] : 0x2D));
        }
        uint64_t previousVersion{};
        for(const auto &region:group->regions) previousVersion=std::max(previousVersion,region.hostVersion);
        // Both old views remain valid. Rejection alone preserves coherence while
        // the live ledger remains; drain plus legacy exclusion loses the next
        // producer despite having published every previous byte successfully.
        operation.Fill(retained.SubView(0,4096),0x6D6D6D6D); operation.Submit();
        assert(buffer->gpuWriteSerial>previousVersion);
        operation.Read(image.get()); operation.Submit();
        const auto encoded=DownloadPartial(image->texture);
        size_t stale{},exclusive{};
        for(size_t i{};i<8192;++i) {
            if(i<4096) exclusive+=encoded[i]!=green[i%4];
            else stale+=encoded[i]!=0x6D;
        }
        assert(!exclusive);
        if(withdraw) {
            assert(stale==4096 && !image->texture->regionStale);
            for(const auto &region:group->regions) assert(region.hostVersion<=previousVersion);
        } else {
            assert(!stale && image->texture->regionAuthority.load()==group);
        }
        buffer->WaitOnFence();
        for(size_t i{4096};i<8192;++i) assert(buffer->backing->data()[i]==0x2D);
        for(size_t i{};i<4096;++i) assert(bytes[i]==0x2D && bytes[16384+i]==0x2D);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("WITHDRAWAL BOUNDARY: drain=%s FreeGuest=%s stale shared bytes=%zu/4096 image-exclusive mismatches=%zu; pending reader preserved; %s\n",
            withdraw?"YES":"NO",eviction?"ON":"OFF",stale,exclusive,withdraw?"RELEASE BLOCKER REPRODUCED":"retained-ledger reference passed");
    }
