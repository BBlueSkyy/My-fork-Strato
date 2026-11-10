// SPDX-License-Identifier: MPL-2.0
// Focused withdrawal proof; not general selective admission for incompatible peers.
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
        bool oldLedgerReleased{false};
        if(withdraw) {
            // A preparation failure must leave the old authority and consumers
            // intact. These are the existing real allocator/record/submit seams.
            for (unsigned failure{}; failure<3; ++failure) {
                operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
                image->texture->downloadStagingBuffer.reset();
                const auto before=group->regions;
                const auto serial=image->texture->gpuWriteSerial;
                gpu.memory.failAllocation=failure==0;
                gpu.scheduler.failRecording=failure==1;
                gpu.scheduler.failSubmit=failure==2;
                bool failed{};
                try { Texture::DrainRegionAuthority({image->texture},{}); } catch(const std::exception &) { failed=true; }
                gpu.memory.failAllocation=gpu.scheduler.failRecording=gpu.scheduler.failSubmit=false;
                assert(failed && group==buffer->regionAuthority.active() && group==image->texture->regionAuthority.active());
                assert(!buffer->regionAuthority.dependency() && !image->texture->regionAuthority.dependency() && image->texture->gpuWriteSerial==serial);
                for(size_t i{};i<before.size();++i) assert(before[i].hostVersion==group->regions[i].hostVersion && before[i].guestVersion==group->regions[i].guestVersion);
                for(const auto &cycle:gpu.scheduler.cycles) assert(cycle->submitted || cycle->cancelled);
            }

            std::weak_ptr<texture::RegionAuthority> oldLedger{group};
            Texture::DrainRegionAuthority({image->texture},{});
            auto replacement{buffer->regionAuthority.dependency()};
            if (replacement) {
                assert(replacement != group && replacement == image->texture->regionAuthority.dependency());
                assert(replacement->dependencyOnly && replacement->guestBridge && !buffer->regionAuthority.active() && !image->texture->regionAuthority.active());
                group.reset(); oldLedgerReleased=oldLedger.expired(); assert(oldLedgerReleased);
                group=std::move(replacement);
            }
            for(size_t i{};i<size;++i) assert(bytes[i]==(i>=4096 && i<12288 ? green[i%4] : 0x2D));
        }
        uint64_t previousVersion{};
        for(const auto &region:group->regions) previousVersion=std::max(previousVersion,region.hostVersion);
        // Both retained views remain valid. The replacement dependency domain
        // must observe the next producer after the active ledger is destroyed.
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
        std::printf("TRANSACTIONAL WITHDRAWAL TARGET: stale shared bytes=%zu/4096 old ledger released=%d\n",stale,oldLedgerReleased);
        assert(!stale);
        if(withdraw) assert(oldLedgerReleased && buffer->regionAuthority.dependency());
        else assert(image->texture->regionAuthority.load()==group);
        buffer->WaitOnFence();
        for(size_t i{4096};i<8192;++i) assert(buffer->backing->data()[i]==0x2D);
        for(size_t i{};i<4096;++i) assert(bytes[i]==0x2D && bytes[16384+i]==0x2D);
        if(withdraw) {
            const auto cancelledWriter=buffer->gpuWriteSerial;
            const auto cancelledNodes=operation.executor.slot->nodes.size();
            bool cancelled{};
            try { operation.Run([&] { operation.executor.PrepareBufferWrite(retained); throw std::runtime_error("cancel prepared dependency use"); }); }
            catch(const std::runtime_error &) { cancelled=true; }
            assert(cancelled && buffer->gpuWriteSerial==cancelledWriter && operation.executor.slot->nodes.size()==cancelledNodes && buffer->regionAuthority.dependency());
            operation.Submit();
            // The replacement participates in later image writers, CPU reads,
            // newly discovered consumers and concrete buffer migration.
            operation.Clear(image.get(),{1,0,0,1}); operation.Submit();
            operation.Read(retained); operation.Submit(); buffer->WaitOnFence();
            const std::array<u8,4> red{255,0,0,255}, blue{0,0,255,255};
            for(size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4096 ? red[i%4] : 0x2D));
            std::vector<u8> cpu(8192);
            operation.Run([&] { buffer->Read(true,[]{ assert(false); },cpu,0); }); operation.Submit();
            for(size_t i{};i<8192;++i) assert(cpu[i]==(i<4096 ? red[i%4] : 0x2D));
            auto consumerDescriptor=PartialDescriptor(bytes+8192); consumerDescriptor.format=format::R8G8B8A8Srgb;
            std::shared_ptr<TextureView> later;
            operation.Run([&] { later=gpu.texture.FindOrCreate(consumerDescriptor,operation.executor.tag); });
            operation.Read(later.get()); operation.Submit();
            auto laterBytes=DownloadPartial(later->texture);
            for(size_t i{};i<8192;++i) assert(laterBytes[i]==(i<4096 ? red[i%4] : 0x2D));
            assert(later->texture->regionAuthority.dependency() && !later->texture->regionAuthority.active());
            operation.Clear(later.get(),{0,0,1,1}); operation.Submit();
            operation.Read(image.get()); operation.Submit(); laterBytes=DownloadPartial(image->texture);
            for(size_t i{};i<8192;++i) assert(laterBytes[i]==(i<4096 ? red[i%4] : blue[i%4]));
            BufferView joined;
            operation.Run([&] { joined=gpu.buffer.FindOrCreate({bytes,4*4096},operation.executor.tag,[&](auto replacement,auto &&lock) {
                operation.executor.AttachLockedBuffer(std::move(replacement),std::move(lock));
            }); });
            operation.Read(joined); operation.Submit(); joined.GetBuffer()->WaitOnFence();
            assert(retained.GetBuffer()==joined.GetBuffer() && retained.GetOffset()==8192);
            assert(joined.GetBuffer()->regionAuthority.dependency() && !joined.GetBuffer()->regionAuthority.active());
            for(size_t i{};i<joined.size;++i) assert(joined.GetBuffer()->backing->data()[i]==(i<4096 ? 0x2D : i<8192 ? red[i%4] : blue[i%4]));
            std::puts("PASS: withdrawn dependency handles allocator/record/submit rollback, future writers/consumers, CPU publication and coalesced intervals");
        }
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("WITHDRAWAL BOUNDARY: drain=%s FreeGuest=%s stale shared bytes=%zu/4096 image-exclusive mismatches=%zu; pending reader preserved; %s\n",
            withdraw?"YES":"NO",eviction?"ON":"OFF",stale,exclusive,withdraw?"persistent-dependency withdrawal passed":"retained-ledger reference passed");
    }
