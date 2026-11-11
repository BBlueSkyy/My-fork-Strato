namespace skyline::gpu {
    void CheckMappedCpuHelpers(GPU &gpu);
    void CheckMappedInline(GPU &gpu) {
        CheckMappedCpuHelpers(gpu);
        constexpr size_t size=4*4096;
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D);
        MixedManagerCleanup cleanup{gpu};
        MixedExecutor operation{gpu};
        soc::gm20b::ChannelContext channel{operation.executor};
        auto &gmmu=channel.asCtx->gmmu;
        gmmu.Map(0x10000,bytes,4096);
        gmmu.afterTranslation=[&] { gmmu.Map(0x10000,bytes+4096,4096); };
        interconnect::Inline2Memory upload{gpu,channel};
        std::vector<u8> value(512,0xA6);
        upload.Upload(0x10000,value);
        operation.Submit();
        auto target=gpu.buffer.FindOrCreate({bytes+4096,512});
        target.GetBuffer()->SynchronizeGuest();
        assert(std::all_of(bytes,bytes+512,[](u8 b) { return b==0x2D; }));
        assert(std::all_of(bytes+4096,bytes+4096+512,[](u8 b) { return b==0xA6; }));
        std::puts("PASS: Inline translation-to-publication remap retries current IOVA");
    }
}
namespace skyline::gpu {
    void CheckMappedDmaCoalesce(GPU &gpu) {
        constexpr size_t size=3*4096;
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        for(size_t i=0;i<size;++i) bytes[i]=static_cast<u8>(i*19+i/127);
        auto expected=std::vector<u8>(bytes,bytes+size); std::memmove(expected.data()+4096,expected.data(),8192);
        MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        soc::gm20b::ChannelContext channel{operation.executor}; channel.asCtx->gmmu.Map(0x10000,bytes,size);
        auto source=gpu.buffer.FindOrCreate({bytes,8192}); auto original=source.GetBuffer()->shared_from_this();
        interconnect::MaxwellDma dma{gpu,channel}; dma.Copy(0x11000,0x10000,8192);
        assert(source.GetBuffer()!=original.get());
        assert(!original->tag.load());
        // Coalescing transfers the replacement lock to the executor before
        // linking delegates, even when this copy can complete on the CPU.
        assert(source.GetBuffer()->tag.load()==operation.executor.tag);
        assert(std::any_of(operation.executor.attachedBuffers.begin(),operation.executor.attachedBuffers.end(),
            [&](const auto &buffer) { return buffer.get()==source.GetBuffer(); }));
        assert(operation.executor.slot->nodes.empty());
        assert(std::equal(expected.begin(),expected.end(),bytes));
        operation.Submit();
        assert(!original->tag.load() && !source.GetBuffer()->tag.load());
        std::puts("PASS: DMA second lookup coalesces locked source; retained concrete locks and memmove stay valid");
    }
    void CheckMappedDma(GPU &gpu) {
        CheckMappedDmaCoalesce(gpu);
        constexpr size_t size=10*4096;
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D);
        MixedManagerCleanup cleanup{gpu};
        MixedExecutor operation{gpu};
        soc::gm20b::ChannelContext channel{operation.executor};
        auto &gmmu=channel.asCtx->gmmu;
        interconnect::MaxwellDma dma{gpu,channel};
        interconnect::Inline2Memory upload{gpu,channel};
        gmmu.Map(0x10000,bytes,4096); gmmu.Map(0x20000,bytes+4096,4096);
        std::fill(bytes+8192,bytes+8192+4096,0x7B);
        gmmu.afterTranslation=[&] { gmmu.Map(0x10000,bytes+8192,4096); gmmu.Map(0x20000,bytes+12288,4096); };
        dma.Copy(0x20000,0x10000,512);
        assert(operation.executor.slot->nodes.empty()); // stable staged CPU copy
        assert(std::all_of(bytes+4096,bytes+4096+512,[](u8 b) { return b==0x2D; }));
        assert(std::all_of(bytes+12288,bytes+12288+512,[](u8 b) { return b==0x7B; }));
        gmmu.afterTranslation=[&] { gmmu.Map(0x20000,bytes+16384,4096); };
        dma.Clear(0x20000,512,0xA6A6A6A6);
        assert(operation.executor.slot->nodes.size()==1);
        operation.Submit();
        auto cleared=gpu.buffer.FindOrCreate({bytes+16384,512}); cleared.GetBuffer()->SynchronizeGuest();
        assert(std::all_of(bytes+16384,bytes+16384+512,[](u8 b) { return b==0xA6; }));
        assert(std::all_of(bytes+12288,bytes+12288+512,[](u8 b) { return b==0x7B; }));
        // The two IOVA pages deliberately map to nonadjacent physical pages.
        gmmu.Map(0x30000,bytes+20480,4096); gmmu.Map(0x31000,bytes+28672,4096);
        gmmu.afterTranslation=[&] { gmmu.Map(0x31000,bytes+36864,4096); };
        std::vector<u8> data(8192); for(size_t i=0;i<data.size();++i) data[i]=static_cast<u8>(i*13+(i/4096)*71);
        upload.Upload(0x30000,data);
        assert(operation.executor.slot->nodes.size()==1);
        operation.Submit();
        for (auto offset : {20480,36864}) { auto v=gpu.buffer.FindOrCreate({bytes+offset,4096}); v.GetBuffer()->SynchronizeGuest(); }
        assert(std::equal(data.begin(),data.begin()+4096,bytes+20480));
        assert(std::equal(data.begin()+4096,data.end(),bytes+36864));
        assert(std::all_of(bytes+28672,bytes+32768,[](u8 b) { return b==0x2D; }));
        // One object, overlapping ranges: CPU memmove and GPU intermediate.
        gmmu.Map(0x40000,bytes+24576,4096);
        std::vector<u8> initial(1024); for(size_t i=0;i<initial.size();++i) initial[i]=static_cast<u8>(i*17+i/13);
        upload.Upload(0x40000,initial);
        auto expected=initial; std::memmove(expected.data()+128,expected.data(),512);
        dma.Copy(0x40080,0x40000,512);
        assert(std::equal(expected.begin(),expected.end(),bytes+24576));
        auto alias=gpu.buffer.FindOrCreate({bytes+24576,1024}); operation.Fill(alias,0x31313131);
        std::vector<u8> patch(512,0xB7); upload.Upload(0x40000,patch);
        dma.Copy(0x40080,0x40000,512);
        operation.Submit(); alias.GetBuffer()->SynchronizeGuest();
        assert(std::all_of(bytes+24576,bytes+24576+640,[](u8 b) { return b==0xB7; }));
        assert(std::all_of(bytes+24576+640,bytes+24576+1024,[](u8 b) { return b==0x31; }));
        operation.Fill(alias,0x31313131);
        patch.resize(511); upload.Upload(0x40001,patch); dma.Copy(0x40081,0x40001,511);
        operation.Submit(); alias.GetBuffer()->SynchronizeGuest();
        assert(bytes[24576]==0x31);
        assert(std::all_of(bytes+24576+1,bytes+24576+640,[](u8 b) { return b==0xB7; }));
        bool rejected{};
        try { dma.Clear(0x40001,4,0); } catch (const std::runtime_error &) { rejected=true; }
        assert(rejected && operation.executor.slot->nodes.empty());
        std::puts("PASS: DMA remap Copy/Clear; atomic fragmented Inline; CPU/GPU overlapping copy");
    }
    void CheckMappedCoherenceRetry(GPU &gpu, int mode=0) {
        if (!mode) { CheckMappedCoherenceRetry(gpu,1); CheckMappedCoherenceRetry(gpu,2); }
        constexpr size_t size=8*4096;
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D);
        MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        soc::gm20b::ChannelContext channel{operation.executor}; auto &gmmu=channel.asCtx->gmmu;
        auto oldImage=gpu.texture.FindOrCreate(PartialDescriptor(bytes));
        auto newImage=gpu.texture.FindOrCreate(PartialDescriptor(bytes+16384));
        operation.Read(oldImage.get()); operation.Read(newImage.get()); operation.Submit();
        auto old=gpu.buffer.FindOrCreate({bytes,4096}); operation.Read(old); operation.Submit();
        operation.Clear(oldImage.get(),{1,0,0,1});
        gmmu.Map(0x10000,bytes,4096);
        unsigned remaps{}; auto submit=operation.executor.onSubmit;
        operation.executor.onSubmit=[&] { submit(); if (!remaps++) gmmu.Map(0x10000,bytes+16384,4096); };
        interconnect::Inline2Memory upload{gpu,channel}; std::vector<u8> data(512,0x6B);
        interconnect::MaxwellDma dma{gpu,channel};
        if (mode==1) {
            std::fill(bytes+8192,bytes+12288,0x6B); gmmu.Map(0x20000,bytes+8192,4096);
            dma.Copy(0x10000,0x20000,512);
        } else if (mode==2) dma.Clear(0x10000,512,0x6B6B6B6B);
        else upload.Upload(0x10000,data);
        assert(remaps>0); assert(operation.executor.slot->nodes.size()==1);
        operation.Submit();
        auto target=gpu.buffer.FindOrCreate({bytes+16384,512}); target.GetBuffer()->PublishRegionGuest();
        old.GetBuffer()->PublishRegionGuest();
        const std::array<u8,4> red{255,0,0,255};
        for(size_t i=0;i<512;++i) { assert(bytes[i]==red[i%4]); assert(bytes[16384+i]==0x6B); }
        operation.executor.onSubmit=submit;
        std::printf("PASS: remap during real mixed coherence retry refreshes %s destination\n",mode==1 ? "DMA Copy" : mode==2 ? "DMA Clear" : "Inline");
    }
}

namespace skyline::gpu {
    void CheckMappedCbufLoad(GPU &gpu) {
        constexpr size_t size=4*4096;
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D);
        MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        soc::gm20b::ChannelContext channel{operation.executor}; auto &gmmu=channel.asCtx->gmmu;
        interconnect::InterconnectContext ctx{gpu,channel,operation.executor};
        interconnect::maxwell3d::ConstantBuffers cbuf;
        gmmu.Map(0x10000,bytes,4096);
        gmmu.afterTranslation=[&] { gmmu.Map(0x10000,bytes+8192,4096); };
        std::vector<u32> data(128,0xB7B7B7B7);
        cbuf.Load(ctx,data,128);
        assert(operation.executor.slot->nodes.empty());
        assert(std::all_of(bytes,bytes+640,[](u8 b) { return b==0x2D; }));
        assert(std::all_of(bytes+8192,bytes+8192+128,[](u8 b) { return b==0x2D; }));
        assert(std::all_of(bytes+8192+128,bytes+8192+640,[](u8 b) { return b==0xB7; }));
        assert(cbuf.selectorState.requested==640);
        auto target=gpu.buffer.FindOrCreate({bytes+8192,4096}); operation.Fill(target,0x31313131);
        cbuf.Load(ctx,data,128);
        operation.Submit(); target.GetBuffer()->SynchronizeGuest();
        assert(std::all_of(bytes+8192,bytes+8192+128,[](u8 b) { return b==0x31; }));
        assert(std::all_of(bytes+8192+128,bytes+8192+640,[](u8 b) { return b==0xB7; }));
        assert(std::all_of(bytes+8192+640,bytes+12288,[](u8 b) { return b==0x31; }));
        bool rejected{};
        try { cbuf.Load(ctx,data,4090); } catch (const std::runtime_error &) { rejected=true; }
        assert(rejected && operation.executor.slot->nodes.empty());
        std::puts("PASS: actual CBUF Load CPU remap retry, offset range and sequenced GPU publication");
    }
}
namespace skyline::gpu {
    void CheckMappedCpuHelpers(GPU &gpu) {
        constexpr size_t size=4096;
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D);
        MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        soc::gm20b::ChannelContext channel{operation.executor}; auto &gmmu=channel.asCtx->gmmu;
        gmmu.Map(0x10000,bytes,size); const auto epoch=gmmu.ReadMappingEpoch();
        auto view=gpu.buffer.FindOrCreate({bytes,size}); auto buffer=view.GetBuffer()->shared_from_this();
        bool badWrite{},badCopy{};
        try { buffer->TryMappedWrite({bytes,0},size+1,operation.executor.usageTracker,epoch); } catch(const std::runtime_error &) { badWrite=true; }
        try { buffer->TryMappedCopy(0,buffer.get(),size+1,0,operation.executor.usageTracker,epoch); } catch(const std::runtime_error &) { badCopy=true; }
        // Exercise direct CPU branch selection with a host mirror. No claim of
        // real Android imported-memory allocation/transport is made by this test.
        buffer->isDirect=true;
        std::vector<u8> data(512,0xA7);
        bool directWrite=buffer->TryMappedWrite(data,0,operation.executor.usageTracker,epoch);
        bool directCopy=buffer->TryMappedCopy(128,buffer.get(),0,512,operation.executor.usageTracker,epoch);
        buffer->isDirect=false;
        std::printf("MAPPED CPU HELPERS: bounds write=%d copy=%d; quiescent direct write=%d copy=%d\n",badWrite,badCopy,directWrite,directCopy);
        assert(badWrite && badCopy && directWrite && directCopy);
        assert(std::all_of(bytes,bytes+640,[](u8 b) { return b==0xA7; }));
        std::puts("PASS: mapped CPU helper bounds and direct quiescent fast-path branch");
    }
}
