// SPDX-License-Identifier: MPL-2.0
namespace skyline::gpu {
    void CheckWithdrawalEvents(GPU &gpu, unsigned event, bool eviction) {
        constexpr size_t size{8*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); std::fill(bytes+16384,bytes+24576,0x73);
        MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        *gpu.state.settings->freeGuestTextureMemory=eviction;
        soc::gm20b::ChannelContext channel{operation.executor}; auto &gmmu=channel.asCtx->gmmu;
        gmmu.Map(0x80000,bytes+4096,8192); const auto oldEpoch=gmmu.ReadMappingEpoch();
        auto descriptor=PartialDescriptor(bytes+4096); descriptor.format=format::R8G8B8A8Srgb;
        auto image=gpu.texture.FindOrCreate(descriptor);
        auto retained=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=retained.GetBuffer()->shared_from_this();
        assert(Texture::RegisterMixedRegionAuthority({image->texture},{buffer},{}));
        operation.Read(image.get()); operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        std::weak_ptr<texture::RegionAuthority> oldDomain{buffer->regionAuthority.load()};
        std::atomic<bool> fired{},blockedNotified{};
        std::promise<void> blocked; auto blocking=blocked.get_future(); std::future<void> writer;
        if(event==0) gmmu.Map(0x80000,bytes+16384,8192);
        else {
            image->texture->mutex.beforeBlocking=[&] { if(!blockedNotified.exchange(true)) blocked.set_value(); };
            verifyRegionalWait=[&] {
                if(fired.exchange(true)) return;
                if(event==1) gmmu.Map(0x80000,bytes+16384,8192);
                else {
                    writer=std::async(std::launch::async,[&] {
                        assert(gpu.state.process->trap.HandleTrap(bytes+8192,true));
                        std::fill(bytes+8192,bytes+8704,0x44);
                    });
                    // The CPU prepare waits outside trapMutex on a full resource
                    // lock. It must restart its old-domain snapshot after commit.
                    assert(blocking.wait_for(std::chrono::seconds(2))==std::future_status::ready);
                }
            };
        }
        Texture::DrainRegionAuthority({image->texture},{});
        if(writer.valid()) writer.get();
        verifyRegionalWait={}; image->texture->mutex.beforeBlocking={};
        assert(oldDomain.expired() && !buffer->regionAuthority.active() && buffer->regionAuthority.dependency());
        assert(image->texture->regionAuthority.dependency()==buffer->regionAuthority.dependency());
        if(event!=0) assert(fired);
        const std::array<u8,4> green{0,255,0,255};
        if(event<2) {
            assert(!oldEpoch.Acquire());
            auto translated=gmmu.TranslateRangeTracked(0x80000,8192);
            assert(translated.fullyResident && translated.mappings.front().data()==bytes+16384 && translated.identity.IsCurrent());
            interconnect::MaxwellDma dma{gpu,channel};
            dma.Clear(0x80000,4096,0x51515151); operation.Submit();
            auto q=gpu.buffer.FindOrCreate({bytes+16384,4096}); q.GetBuffer()->SynchronizeGuest();
            assert(!q.GetBuffer()->regionAuthority.load()); // Q is a different physical component.
            for(size_t i{};i<8192;++i) assert(bytes[16384+i]==(i<4096 ? 0x51 : 0x73));
            operation.Fill(retained.SubView(0,4096),0x6D6D6D6D); operation.Submit();
        }
        operation.Read(image.get()); operation.Submit();
        const auto encoded=DownloadPartial(image->texture);
        for(size_t i{};i<8192;++i) {
            const u8 expected=i<4096 ? green[i%4] : event<2 ? 0x6D : i<4608 ? 0x44 : green[i%4];
            assert(encoded[i]==expected);
        }
        std::vector<u8> cpu(8192);
        operation.Run([&] { buffer->Read(true,[]{ assert(false); },cpu,0); }); operation.Submit();
        for(size_t i{};i<8192;++i) {
            const u8 expected=i>=4096 ? 0x2D : event<2 ? 0x6D : i<512 ? 0x44 : green[i%4];
            assert(cpu[i]==expected);
        }
        if(event<2) for(size_t i{};i<8192;++i) assert(bytes[16384+i]==(i<4096 ? 0x51 : 0x73));
        for(size_t i{};i<4096;++i) assert(bytes[i]==0x2D && bytes[24576+i]==0x2D);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS: dependency withdrawal event=%s FreeGuest=%s; physical identity, future bytes, exclusive bytes and CPU publication preserved\n",
            event==0 ? "remap-before" : event==1 ? "remap-during" : "concurrent-CPU",eviction?"ON":"OFF");
    }
}

namespace skyline::gpu {
    void CheckWithdrawalRegistrationSnapshot(GPU &gpu) {
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        auto descriptor=PartialDescriptor(bytes+4096); descriptor.format=format::R8G8B8A8Srgb;
        auto image=gpu.texture.FindOrCreate(descriptor);
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=view.GetBuffer()->shared_from_this();
        assert(Texture::RegisterMixedRegionAuthority({image->texture},{buffer},{}));
        operation.Read(image.get()); operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        auto newcomer=std::make_shared<Texture>(gpu,descriptor); newcomer->SetupGuestMappings();
        auto old=buffer->regionAuthority.load();
        std::unique_lock hold{*buffer};
        const auto references=old.use_count();
        auto join=std::async(std::launch::async,[&] {
            try { return std::pair{Texture::RegisterMixedRegionAuthority({image->texture,newcomer},{buffer},{}),false}; }
            catch(const TextureAliasReconciliationRequest &) { return std::pair{false,true}; }
        });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(old.use_count()<=references && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        assert(old.use_count()>references); // Registration retains its old component snapshot before resource acquisition.
        Texture::DrainRegionAuthority({image->texture},{});
        operation.Fill(view.SubView(0,4096),0x6D6D6D6D); operation.Submit();
        hold.unlock();
        const auto [admitted,retry]=join.get();
        assert(admitted || retry);
        if(retry) assert(Texture::RegisterMixedRegionAuthority({image->texture,newcomer},{buffer},{}));
        AliasRead(newcomer); const auto encoded=DownloadPartial(newcomer);
        const std::array<u8,4> green{0,255,0,255}; size_t stale{};
        for(size_t i{};i<8192;++i) stale+=encoded[i]!=(i<4096 ? green[i%4] : 0x6D);
        std::printf("WITHDRAWAL REGISTRATION SNAPSHOT: stale=%zu/4096 retried=%d\n",stale,retry);
        assert(!stale && retry && newcomer->regionAuthority.dependency() && !newcomer->regionAuthority.active());
        gpu.scheduler.Complete(); assert(!validationErrors);
    }
}
