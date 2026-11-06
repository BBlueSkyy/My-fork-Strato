// SPDX-License-Identifier: MPL-2.0
// Included in the real-Vulkan fixture; not a replacement authority model.
    void CheckBufferImageSectors(GPU &gpu, bool eviction) {
        *gpu.state.settings->freeGuestTextureMemory = eviction;
        constexpr size_t extentBytes{32 * 32 * 4}, guard{64};
        constexpr vk::Extent3D extent{32,32,1};
        using texture::RawRgba8Gob;
        using texture::PlanRawRgba8GobCopies;
        std::vector<u8> guest(extentBytes,0x2D);
        auto descriptor{PartialDescriptor(guest.data())};
        descriptor.dimensions=extent; descriptor.layerStride=extentBytes;
        descriptor.mappings={{guest.data(),guest.size()}};
        auto image{std::make_shared<Texture>(gpu,descriptor)};
        image->SetupGuestMappings(); image->SynchronizeHost(false);
        const auto native{image->GetBacking()};
        image->RecordGpuWrite(); image->SynchronizeHost(true);
        assert(image->memoryFreed==eviction);
        std::vector<RawRgba8Gob> gobs;
        // Independent physical GOB ordering (BH=1): y-major, then x-major.
        for(u32 y{};y<32;y+=8) for(u32 x{};x<32;x+=16)
            gobs.push_back({guard + (y/8*2+x/16)*512,x,y});
        auto full{PlanRawRgba8GobCopies(gobs,extentBytes+2*guard,extent)};
        assert(full && full->size()==256);
        // Deliberately nonuniform sectors: a uniform fill cannot detect swizzle bugs.
        std::vector<u8> raw(extentBytes+2*guard,0xD7), expected(extentBytes);
        for(size_t y{};y<32;++y) for(size_t x{};x<128;++x) {
            const u8 value{static_cast<u8>((x*13+y*29+(x/4)*7)^0x5B)};
            // Byte address oracle, independent of the planner's sector loop.
            const size_t address{(y/8*2+x/64)*512 + ((x&32)<<3) + ((y&6)<<5) +
                ((x&16)<<1) + ((y&1)<<4) + (x&15)};
            raw[guard+address]=value; expected[y*128+x]=value;
        }
        TransportBuffer seed{gpu,raw.size()}, producer{gpu,raw.size()}, result{gpu,raw.size()};
        seed.Write(raw); producer.Write(std::vector<u8>(raw.size(),0xB4));
        result.Write(std::vector<u8>(raw.size(),0xA6));
        // The image consumes a real GPU buffer producer, not the host seed.
        auto rawWrite{gpu.scheduler.Submit([&](auto &commands) {
            const vk::BufferCopy copy{.srcOffset=0,.dstOffset=0,.size=raw.size()};
            commands.copyBuffer(*seed.buffer,*producer.buffer,copy);
        })};
        rawWrite->WaitSubmit();
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
        auto barrier=[&](auto &commands, bool write) {
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},
                vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask=vk::AccessFlagBits::eTransferRead|vk::AccessFlagBits::eTransferWrite},{},
                vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask=write ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eTransferRead,
                    .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                    .image=native,.subresourceRange=range});
        };
        if(image->cycle) image->cycle->WaitSubmit();
        auto upload{gpu.scheduler.Submit([&](auto &commands) {
            barrier(commands,true);
            if(std::getenv("BUFFER_IMAGE_LINEAR_NEGATIVE")) {
                const vk::BufferImageCopy incorrect{.bufferOffset=guard,
                    .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent=extent};
                commands.copyBufferToImage(*producer.buffer,native,vk::ImageLayout::eGeneral,incorrect);
            } else commands.copyBufferToImage(*producer.buffer,native,vk::ImageLayout::eGeneral,*full);
        })};
        upload->WaitSubmit();
        auto linear{gpu.memory.AllocateStagingBuffer(extentBytes)};
        auto verify{gpu.scheduler.Submit([&](auto &commands) { image->CopyIntoStagingBuffer(commands,linear); })};
        verify->Wait();
        assert(std::equal(expected.begin(),expected.end(),linear->data()));
        // A second producer changes one overlapping GOB, preserving all others.
        const std::vector<RawRgba8Gob> partial{{guard+3*512,16,8}};
        auto one{PlanRawRgba8GobCopies(partial,raw.size(),extent)};
        assert(one && one->size()==32);
        TransportBuffer second{gpu,raw.size()};
        auto replacement{raw};
        for(size_t i{};i<512;++i) replacement[guard+3*512+i]^=0x93;
        second.Write(replacement);
        auto overwrite{gpu.scheduler.Submit([&](auto &commands) {
            barrier(commands,true);
            commands.copyBufferToImage(*second.buffer,native,vk::ImageLayout::eGeneral,*one);
        })};
        overwrite->WaitSubmit();
        // Partial image write in a different GOB. Native linear staging is an
        // independent producer; the inverse plan must preserve raw-sector order.
        auto pixels{gpu.memory.AllocateStagingBuffer(16*8*4)};
        for(size_t i{};i<512;++i) pixels->data()[i]=static_cast<u8>(i*31+0x27);
        auto imageWrite{gpu.scheduler.Submit([&](auto &commands) {
            barrier(commands,true);
            const vk::BufferImageCopy write{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .imageOffset={0,16,0},.imageExtent={16,8,1}};
            commands.copyBufferToImage(pixels->vkBuffer,native,vk::ImageLayout::eGeneral,write);
        })};
        imageWrite->WaitSubmit();
        auto download{gpu.scheduler.Submit([&](auto &commands) {
            barrier(commands,false);
            commands.copyImageToBuffer(native,vk::ImageLayout::eGeneral,*result.buffer,*full);
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{}, {},
                vk::BufferMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask=vk::AccessFlagBits::eHostRead,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=*result.buffer,.offset=0,.size=raw.size()},{});
        })};
        download->Wait();
        auto actual{result.Read()};
        for(size_t i{};i<actual.size();++i) {
            u8 value{0xA6};
            if(i>=guard && i<guard+extentBytes) {
                const auto offset{i-guard}; value=replacement[i];
                if(offset>=4*512 && offset<5*512) {
                    // Independent inverse lookup for the changed image GOB.
                    bool found{};
                    for(size_t y{};y<8;++y) for(size_t x{};x<64;++x) {
                        const size_t morton{((x&32)<<3)+((y&6)<<5)+((x&16)<<1)+((y&1)<<4)+(x&15)};
                        if(morton==offset-4*512) { value=pixels->data()[y*64+x]; found=true; }
                    }
                    assert(found);
                }
            }
            assert(actual[i]==value);
        }
        // Reject every unsupported footprint before it can reach Vulkan.
        assert(!PlanRawRgba8GobCopies({{guard,1,0}},raw.size(),extent));
        assert(!PlanRawRgba8GobCopies({{guard,0,1}},raw.size(),extent));
        assert(!PlanRawRgba8GobCopies({{guard,32,0}},raw.size(),extent));
        assert(!PlanRawRgba8GobCopies({{guard+1,0,0}},raw.size(),extent));
        assert(!PlanRawRgba8GobCopies({{raw.size()-511,0,0}},raw.size(),extent));
        assert(!PlanRawRgba8GobCopies({{UINT64_MAX-255,0,0}},UINT64_MAX,extent));
        assert(!PlanRawRgba8GobCopies({{guard,0,0}},raw.size(),vk::Extent3D{32,32,2}));
        assert(!PlanRawRgba8GobCopies({{guard,0,0},{guard,16,0}},raw.size(),extent));
        assert(!PlanRawRgba8GobCopies({{guard,0,0},{guard+512,0,0}},raw.size(),extent));
        std::vector<RawRgba8Gob> boundary;
        for(u32 i{};i<129;++i)boundary.push_back({i*512ULL,(i%16)*16,(i/16)*8});
        assert(!PlanRawRgba8GobCopies(boundary,129*512,vk::Extent3D{256,72,1}));
        boundary.pop_back();
        assert(PlanRawRgba8GobCopies(boundary,128*512,vk::Extent3D{256,64,1})->size()==4096);
        assert(!PlanRawRgba8GobCopies({{0,0,0}},512,vk::Extent3D{UINT32_MAX,32,1}));
        assert(PlanRawRgba8GobCopies({},0,extent)->empty());
        image->cycle=download;
        image->SynchronizeGuest(false,true);
        assert(!image->memoryFreed);
        assert(std::equal(guest.begin(),guest.end(),actual.begin()+guard));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS: raw-sector Buffer -> Image -> Buffer, partial GOB producers, preserved bytes, bounds; Free Guest %s\n",eviction?"ON":"OFF");
    }

    // The scheduler and host mapping/trap boundaries are facades. Every manager
    // registration, authority decision, copy, publication and writer commit below
    // executes the extracted production bodies against real Vulkan storage.
    struct MixedExecutor {
        GPU &gpu;
        interconnect::CommandExecutor executor;
        explicit MixedExecutor(GPU &gpu) : gpu{gpu} {
            executor.onSubmit = [&] {
                bool commands{};
                for (const auto &node : executor.slot->nodes)
                    commands |= std::holds_alternative<interconnect::node::FunctionNode>(node);
                std::shared_ptr<FenceCycle> cycle;
                if (commands) {
                    auto active=gpu.scheduler.AllocateCommandBuffer();
                    cycle=active.GetFenceCycle();
                    auto &cb=*active;
                    cb.begin(vk::CommandBufferBeginInfo{.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
                    {
                        for (const auto &image : executor.attachedTextures) image->SynchronizeHostInline(cb,cycle,true);
                        for (const auto &buffer : executor.attachedBuffers) buffer->SynchronizeHost();
                        cb.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,{},
                            vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryWrite,
                                .dstAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite},{},{});
                        for (const auto &node : executor.slot->nodes)
                            if (const auto *function=std::get_if<interconnect::node::FunctionNode>(&node)) function->function(cb,cycle,gpu);
                        cb.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,{},
                            vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryWrite,
                                .dstAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite},{},{});
                    }
                    cb.end(); gpu.scheduler.SubmitCommandBuffer(cb,cycle);
                }
                for (auto &image : executor.attachedTextures) {
                    if (cycle) { image->cycle=cycle; cycle->AttachObject(image); }
                    image->pendingRegionUseTag={}; image->unlock();
                }
                for (auto &buffer : executor.attachedBuffers) {
                    if (cycle) { buffer->UpdateCycle(cycle); cycle->AttachObject(buffer); }
                    buffer->pendingRegionUseTag={}; buffer->AllowAllBackingWrites(); buffer->unlock();
                }
                executor.attachedTextures.clear(); executor.attachedBuffers.clear();
            };
        }
        ~MixedExecutor() { executor.Submit(); }
        template<class F> void Run(F &&operation) {
            unsigned attempts{};
            executor.RunWithTextureBackingRetry([&] {
                if (++attempts>8) throw std::runtime_error("mixed operation did not converge within eight retries");
                operation();
            });
        }
        void Read(BufferView view) { Run([&] { executor.AttachBuffer(view); view.GetBuffer()->SynchronizeHost(); }); }
        void Read(TextureView *view) { view->lockOnAttach=true; Run([&] { executor.AttachTexture(view); view->texture->SynchronizeHost(false); }); }
        void Fill(BufferView view, u32 value) {
            Run([&] {
                executor.AddOutsideRpCommand([view,value](auto &cb,const auto &,auto &) {
                    cb.fillBuffer(view.GetBuffer()->GetBacking(),view.GetOffset(),view.size,value);
                },{},span<BufferView>{view});
            });
        }
        void Clear(TextureView *view, std::array<float,4> value) {
            view->lockOnAttach=true;
            Run([&] {
                executor.AddOutsideRpCommand([view,value](auto &cb,const auto &,auto &) {
                    cb.clearColorImage(view->texture->GetBacking(),view->texture->layout,vk::ClearColorValue{value},view->range);
                },span<TextureView *>{view});
            });
        }
        void Submit() { executor.Submit(); }
    };
    struct MixedManagerCleanup {
        GPU &gpu;
        ~MixedManagerCleanup() {
            gpu.scheduler.Complete();
            gpu.texture.textures.clear(); gpu.texture.overlaps={}; ++gpu.texture.regionRevision;
            gpu.buffer.bufferMappings.clear(); gpu.buffer.bufferTable.entries.clear(); ++gpu.buffer.regionRevision;
            gpu.buffer.delegateAllocatorState.objects.clear();
        }
    };
    void CheckAutomaticMixed(GPU &gpu, bool eviction, texture::Format nativeFormat = format::R8G8B8A8Unorm) {
        if (!std::getenv("TEXMAN_MIXED_GATE_OFF")) assert(texture::MixedRegionAuthorityEnabled); // Only the isolated header copy is enabled.
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D);
        MixedManagerCleanup cleanup{gpu};
        *gpu.state.settings->freeGuestTextureMemory=eviction;
        MixedExecutor operation{gpu};
        // The two resources overlap by 4096 bytes and each has exclusive bytes.
        // sRGB shares bytes and GOB geometry with UNORM; 0.0/1.0 clears encode to
        // 0/255 in both, so the byte oracles below are format independent.
        auto image=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096,nativeFormat));
        assert(image->texture->format==nativeFormat && image->texture->format->vkFormat==nativeFormat->vkFormat);
        operation.Read(image.get()); operation.Clear(image.get(),{1,0,0,1}); operation.Submit();
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192});
        auto buffer=view.GetBuffer()->shared_from_this();
        operation.Read(view); operation.Submit();
        assert(image->texture->regionAuthority.load()==buffer->regionAuthority.load());
        assert(image->texture->regionTracked && buffer->regionTracked);
        buffer->WaitOnFence();
        const std::array<u8,4> red{255,0,0,255};
        for (size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4096 ? red[i%4] : 0x2D));
        // A sub-GOB producer must restore the other 496 bytes before claiming
        // its authority cell. A second disjoint range covers buffer-only bytes.
        auto sub=buffer->GetView(16,16), exclusive=buffer->GetView(4096,4096);
        operation.Fill(sub,0x88776655); operation.Fill(exclusive,0xB4B4B4B4); operation.Submit();
        operation.Read(image.get()); operation.Submit();
        auto encoded=DownloadPartial(image->texture);
        for (size_t i{};i<encoded.size();++i) {
            const auto expected=(i>=4112 && i<4128) ? std::array<u8,4>{0x55,0x66,0x77,0x88}[i%4] : red[i%4];
            if (encoded[i]!=expected) { std::fprintf(stderr,"MIXED MISMATCH offset=%zu actual=%u expected=%u buffer=%u\n",i,unsigned(encoded[i]),unsigned(expected),unsigned(buffer->backing->data()[i>=4096 ? i-4096 : 0])); std::abort(); }
        }
        // Cached views invoke preparation directly; no FindOrCreate is needed.
        operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        operation.Read(view); operation.Submit(); buffer->WaitOnFence();
        const std::array<u8,4> green{0,255,0,255};
        for (size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4096 ? green[i%4] : 0xB4));
        // The CPU reader must publish all current owners before consuming mirror
        // bytes; selective buffer publication cannot clobber image-owned bytes.
        std::vector<u8> cpu(8192);
        operation.Run([&] { buffer->Read(true,[]{ assert(false); },cpu,0); }); operation.Submit();
        for (size_t i{};i<8192;++i) assert(cpu[i]==(i<4096 ? green[i%4] : 0xB4));
        for (size_t i{};i<size;++i) {
            const u8 expected=i<4096 || i>=16384 ? 0x2D : i<12288 ? green[i%4] : 0xB4;
            assert(bytes[i]==expected);
        }
        const auto submissions=gpu.scheduler.submissions;
        const auto group=buffer->regionAuthority.load();
        const auto allocationCount=gpu.memory.count, stagingCount=gpu.memory.stagingCount;
        const auto trapChanges=gpu.state.process->trap.changes.load();
        auto waits=[&] { unsigned result{}; for (const auto &cycle : gpu.scheduler.cycles) result+=cycle->gpuWaits+cycle->submitWaits; return result; };
        const auto waitCount=waits();
        for (unsigned i{};i<32;++i) { operation.Read(view); operation.Read(image.get()); }
        operation.Submit();
        assert(buffer->regionAuthority.load()==group && gpu.scheduler.submissions==submissions && gpu.memory.count==allocationCount && gpu.memory.stagingCount==stagingCount);
        assert(waits()==waitCount && gpu.state.process->trap.changes.load()==trapChanges);
        std::puts("STABLE COUNTS (32 buffer + 32 image cached preparations): extra submissions=0, image allocations=0, staging allocations=0, fence waits=0, trap changes=0, group replacements=0");
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS: automatic production managers + executor Buffer <-> Image, sub-GOB writes, exclusive bytes, CPU publication, cached reads, 32 stable uses; format=%s FreeGuest=%s\n",vk::to_string(nativeFormat->vkFormat).c_str(),eviction?"ON":"OFF");
    }

    void CheckMixedFailures(GPU &gpu, bool eviction) {
        constexpr size_t size{8*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu};
        *gpu.state.settings->freeGuestTextureMemory=eviction;
        MixedExecutor operation{gpu};
        auto image=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096));
        operation.Read(image.get()); operation.Clear(image.get(),{1,0,0,1}); operation.Submit();
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=view.GetBuffer()->shared_from_this();
        operation.Read(view); operation.Submit();
        // Buffer->Image transfer rollback at recording, dependency attachment,
        // and queue submission boundaries. Retries must retain exact old versions.
        for (unsigned failure{};failure<3;++failure) {
            operation.Fill(buffer->GetView(0,512),0x56565656+failure*0x11111111); operation.Submit();
            const auto bindings=image->texture->regionBindings;
            const auto serial=buffer->gpuWriteSerial;
            const auto writer=image->texture->gpuWriteSerial;
            gpu.scheduler.failRecording=failure==0; FenceCycle::failAttachment=failure==1; gpu.scheduler.failSubmit=failure==2;
            bool rejected{};
            try { operation.Read(image.get()); } catch (const std::exception &) { rejected=true; }
            gpu.scheduler.failRecording=false; FenceCycle::failAttachment=false; gpu.scheduler.failSubmit=false;
            assert(rejected && buffer->gpuWriteSerial==serial && image->texture->gpuWriteSerial==writer);
            assert(image->texture->regionStale && bindings.size()==image->texture->regionBindings.size());
            for (size_t i{};i<bindings.size();++i) assert(bindings[i].residentVersion==image->texture->regionBindings[i].residentVersion);
            operation.Read(image.get()); operation.Submit();
            const auto encoded=DownloadPartial(image->texture);
            for (size_t i{};i<512;++i) assert(encoded[4096+i]==0x56+failure*0x11);
        }
        // Image->Buffer transfer has its own transaction and failure cleanup.
        for (unsigned failure{};failure<3;++failure) {
            operation.Clear(image.get(),{0,0,1,1}); operation.Submit();
            const auto bindings=buffer->regionBindings;
            gpu.scheduler.failRecording=failure==0; FenceCycle::failAttachment=failure==1; gpu.scheduler.failSubmit=failure==2;
            bool rejected{};
            try { operation.Read(view); } catch (const std::exception &) { rejected=true; }
            gpu.scheduler.failRecording=false; FenceCycle::failAttachment=false; gpu.scheduler.failSubmit=false;
            assert(rejected && buffer->regionStale);
            for (size_t i{};i<bindings.size();++i) assert(bindings[i].residentVersion==buffer->regionBindings[i].residentVersion);
            operation.Read(view); operation.Submit(); buffer->WaitOnFence();
            const std::array<u8,4> blue{0,0,255,255};
            for (size_t i{};i<4096;++i) assert(buffer->backing->data()[i]==blue[i%4]);
        }
        // A descriptor failure after writable buffer preparation is not a writer.
        const auto serial=buffer->gpuWriteSerial;
        const auto nodes=operation.executor.slot->nodes.size();
        bool rejected{};
        try { operation.Run([&] { operation.executor.PrepareBufferWrite(view); throw std::bad_alloc{}; }); }
        catch (const std::bad_alloc &) { rejected=true; }
        assert(rejected && buffer->gpuWriteSerial==serial && operation.executor.slot->nodes.size()==nodes);
        operation.Submit();
        // Two writers sharing a physical GOB are rejected before publication.
        const auto imageSerial=image->texture->gpuWriteSerial;
        auto imagePointer=image.get();
        rejected=false;
        try { operation.Run([&] { operation.executor.AddOutsideRpCommand([](auto &,const auto &,auto &) {},span<TextureView *>{&imagePointer,1},span<BufferView>{view}); }); }
        catch (const std::runtime_error &) { rejected=true; }
        assert(rejected && buffer->gpuWriteSerial==serial && image->texture->gpuWriteSerial==imageSerial && operation.executor.slot->nodes.size()==nodes);
        operation.Submit();
        // CPU publication allocation failure must leave the image authoritative;
        // retry publishes the current blue image plus the buffer's exclusive data.
        operation.Fill(buffer->GetView(4096,4096),0xC3C3C3C3); operation.Submit();
        image->texture->downloadStagingBuffer.reset(); gpu.memory.failAllocation=true;
        std::vector<u8> cpu(8192); rejected=false;
        try { operation.Run([&] { buffer->Read(true,[]{ assert(false); },cpu,0); }); }
        catch (const std::bad_alloc &) { rejected=true; }
        gpu.memory.failAllocation=false;
        assert(rejected && image->texture->NeedsRegionReadback());
        operation.Run([&] { buffer->Read(true,[]{ assert(false); },cpu,0); }); operation.Submit();
        const std::array<u8,4> blue{0,0,255,255};
        for (size_t i{};i<8192;++i) assert(cpu[i]==(i<4096 ? blue[i%4] : 0xC3));
        // Real staged-buffer trap callbacks publish the common component, then
        // release only the CPU-written buffer cells before GPU consumers resume.
        gpu.state.process->trap.reverseEntries=eviction;
        assert(gpu.state.process->trap.HandleTrap(bytes+8192,false));
        assert(gpu.state.process->trap.HandleTrap(bytes+8192,true));
        std::fill(bytes+8192,bytes+12288,0x6A);
        operation.Read(image.get()); operation.Submit();
        const auto encoded=DownloadPartial(image->texture);
        for (size_t i{};i<8192;++i) assert(encoded[i]==(i<4096 ? blue[i%4] : 0x6A));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS: mixed directional recording/attach/submit rollback+retry, deferred descriptor writer, simultaneous-writer rejection, CPU allocation recovery and staged traps; FreeGuest=%s\n",eviction?"ON":"OFF");
    }

    void CheckMixedCoalesce(GPU &gpu, bool independent) {
        constexpr size_t size{10*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu};
        MixedExecutor operation{gpu};
        auto image=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096));
        operation.Read(image.get()); operation.Clear(image.get(),{1,0,0,1}); operation.Submit();
        auto retained=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto old=retained.GetBuffer()->shared_from_this();
        operation.Read(retained); operation.Fill(old->GetView(4096,4096),0xB4B4B4B4); operation.Submit();
        operation.Clear(image.get(),{0,1,0,1}); operation.Submit(); // old backing intentionally stale
        std::shared_ptr<TextureView> secondImage; BufferView secondView;
        std::shared_ptr<Buffer> secondOld;
        if (independent) {
            secondImage=gpu.texture.FindOrCreate(PartialDescriptor(bytes+5*4096));
            operation.Read(secondImage.get()); operation.Clear(secondImage.get(),{0,0,1,1}); operation.Submit();
            secondView=gpu.buffer.FindOrCreate({bytes+6*4096,8192}); secondOld=secondView.GetBuffer()->shared_from_this();
            operation.Read(secondView); operation.Fill(secondOld->GetView(4096,4096),0xC3C3C3C3); operation.Submit();
            assert(old->regionAuthority.load()!=secondOld->regionAuthority.load());
        }
        BufferView joined;
        operation.Run([&] { joined=gpu.buffer.FindOrCreate({bytes,independent ? 8*4096UL : 4*4096UL},operation.executor.tag); });
        assert(joined && joined.GetBuffer()!=old.get() && retained.GetBuffer()==joined.GetBuffer() && retained.GetOffset()==8192);
        assert(!old->regionTracked && !old->regionAuthority.load());
        if (independent) assert(secondView.GetBuffer()==joined.GetBuffer() && !secondOld->regionTracked);
        operation.Read(joined); operation.Submit(); auto target=joined.GetBuffer()->shared_from_this(); target->WaitOnFence();
        const std::array<u8,4> green{0,255,0,255}, blue{0,0,255,255};
        for (size_t i{};i<joined.size;++i) {
            const u8 expected=i>=4096 && i<3*4096 ? green[i%4] : i>=3*4096 && i<4*4096 ? 0xB4 :
                independent && i>=5*4096 && i<7*4096 ? blue[i%4] : independent && i>=7*4096 ? 0xC3 : 0x2D;
            assert(target->backing->data()[i]==expected);
        }
        auto group=target->regionAuthority.load();
        for (const auto &region : group->regions) assert(region.bufferWriterIdentity!=old.get() && (!secondOld || region.bufferWriterIdentity!=secondOld.get()));
        operation.Fill(retained,0x5E5E5E5E); operation.Submit(); operation.Read(image.get()); operation.Submit();
        const auto encoded=DownloadPartial(image->texture);
        for (size_t i{};i<8192;++i) assert(encoded[i]==(i<4096 ? green[i%4] : 0x5E));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS: automatic mixed coalesce, stale foreign image cells, buffer-exclusive producers, retained delegates, postmerge writer; independent components=%s\n",independent?"YES":"NO");
    }

    void CheckMixedLatePreparation(GPU &gpu) {
        constexpr size_t size{8*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        auto mixed=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096));
        operation.Read(mixed.get()); operation.Submit();
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192}); operation.Read(view); operation.Submit();
        auto other=gpu.texture.FindOrCreate(PartialDescriptor(bytes+5*4096)); operation.Read(other.get()); operation.Submit();
        assert(mixed->texture->regionTracked && !other->texture->regionTracked);
        const auto mixedSerial=mixed->texture->gpuWriteSerial, otherSerial=other->texture->gpuWriteSerial;
        const auto nodes=operation.executor.slot->nodes.size();
        std::array<TextureView *,2> writers{mixed.get(),other.get()};
        auto &intervals=operation.executor.usageTracker.dirtyIntervals;
        intervals.failAt=intervals.count+2;
        bool rejected{};
        try { operation.Run([&] { operation.executor.PreflightTextureUses({},writers); }); }
        catch (const std::bad_alloc &) { rejected=true; }
        intervals.failAt={};
        std::fprintf(stderr,"LATE PREPARATION: rejected=%d mixedSerial=%llu->%llu otherSerial=%llu->%llu nodes=%zu->%zu\n",rejected,
            static_cast<unsigned long long>(mixedSerial),static_cast<unsigned long long>(mixed->texture->gpuWriteSerial),
            static_cast<unsigned long long>(otherSerial),static_cast<unsigned long long>(other->texture->gpuWriteSerial),nodes,operation.executor.slot->nodes.size());
        assert(rejected && mixed->texture->gpuWriteSerial==mixedSerial && other->texture->gpuWriteSerial==otherSerial && operation.executor.slot->nodes.size()==nodes);
        operation.Submit(); gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: late second image dirty-interval preparation failure publishes no mixed writer/node");
    }

    void CheckMixedMultipleProducers(GPU &gpu) {
        constexpr size_t size{8*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        auto a=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096)); operation.Read(a.get()); operation.Clear(a.get(),{1,0,0,1}); operation.Submit();
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=view.GetBuffer()->shared_from_this(); operation.Read(view); operation.Submit();
        std::shared_ptr<TextureView> b; operation.Run([&] { b=gpu.texture.FindOrCreate(PartialDescriptor(bytes+3*4096),operation.executor.tag); }); operation.Read(b.get()); operation.Clear(b.get(),{0,0,1,1}); operation.Submit();
        assert(a->texture->regionAuthority.load()==b->texture->regionAuthority.load() && a->texture->regionAuthority.load()==buffer->regionAuthority.load());
        operation.Read(view); operation.Submit(); buffer->WaitOnFence();
        const std::array<u8,4> red{255,0,0,255},blue{0,0,255,255},green{0,255,0,255};
        for (size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4096 ? red[i%4] : blue[i%4]));
        // A pending cached-buffer consumer must see the old producers before a
        // later image writer invalidates/reconciles that buffer's host storage.
        auto readback=gpu.memory.AllocateStagingBuffer(8192);
        operation.Run([&] {
            operation.executor.AttachBuffer(view);
            operation.executor.AddOutsideRpCommand([view,readback](auto &cb,const auto &,auto &) {
                cb.copyBuffer(view.GetBuffer()->GetBacking(),readback->vkBuffer,vk::BufferCopy{.size=8192});
            });
        });
        operation.Clear(a.get(),{0,1,0,1}); // Queued after consumer; not submitted yet.
        assert(!operation.executor.slot->nodes.empty());
        operation.Read(view); operation.Submit(); buffer->WaitOnFence();
        for (size_t i{};i<8192;++i) {
            assert(readback->data()[i]==(i<4096 ? red[i%4] : blue[i%4]));
            assert(buffer->backing->data()[i]==(i<4096 ? green[i%4] : blue[i%4]));
        }
        operation.Fill(buffer->GetView(16,16),0x78787878);
        operation.Fill(buffer->GetView(4112,16),0x9A9A9A9A); operation.Submit();
        operation.Read(a.get()); operation.Read(b.get()); operation.Submit();
        const auto encodedA=DownloadPartial(a->texture), encodedB=DownloadPartial(b->texture);
        for (size_t i{};i<8192;++i) {
            assert(encodedA[i]==(i>=4112 && i<4128 ? 0x78 : green[i%4]));
            assert(encodedB[i]==(i>=16 && i<32 ? 0x9A : blue[i%4]));
        }
        // An IOVA epoch change cannot erase physical producer history. Both
        // views remain physical aliases and retain the same shared authority.
        const auto group=buffer->regionAuthority.load();
        auto identity=std::make_shared<MappingIdentity>();
        auto lease=identity->Capture(0x1000,8192);
        bool rejected{};
        operation.Run([&] {
            operation.executor.TrackTextureMapping(lease); operation.executor.AttachBuffer(view);
            { auto mutation=identity->Invalidate(0x1000,8192); }
            try { auto publication=operation.executor.AcquireRegionPublication({}); } catch (const TextureAliasReconciliationRequest &) { rejected=true; }
        });
        assert(rejected && buffer->regionAuthority.load()==group);
        operation.Submit(); gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: two independent image producers + raw buffer sub-GOB producer, queued old-buffer consumer, retained aliases and stale mapping lease");
    }


    void CheckMixedGuestBridge(GPU &gpu) {
        assert(texture::MixedRegionAuthorityEnabled && texture::MixedGuestBridgeExperimental);
        constexpr size_t allocation{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,allocation,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,allocation); });
        std::fill(bytes,bytes+allocation,0x2D);
        MixedManagerCleanup cleanup{gpu};
        *gpu.state.settings->freeGuestTextureMemory=false;
        MixedExecutor op{gpu};
        auto descriptor{PartialDescriptor(bytes+4096)};
        descriptor.dimensions={20,12,1}; // 80-byte rows, partial right and bottom GOBs.
        auto image{gpu.texture.FindOrCreate(descriptor)};
        op.Read(image.get()); op.Clear(image.get(),{1,0,0,1}); op.Submit();
        auto view{gpu.buffer.FindOrCreate({bytes+4096,4096})};
        auto buffer{view.GetBuffer()->shared_from_this()};
        op.Read(view); op.Submit();
        auto group{buffer->regionAuthority.load()};
        assert(group && group==image->texture->regionAuthority.load());
        assert(group->transport==texture::RegionTransport::GuestBridge);
        buffer->WaitOnFence();
        const auto *host{buffer->backing->data()};
        // Independent 20x12 block-linear byte oracle: actual encoder bytes
        // are red while all edge sectors, other GOB rows and buffer padding
        // must remain the original guest fill.
        std::vector<u8> expected(4096,0x2D);
        for (size_t y{};y<12;++y) for (size_t x{};x<80;++x) {
            const size_t offset{((y/8)*2+x/64)*512 + ((x&32)<<3) +
                ((y&6)<<5) + ((x&16)<<1) + ((y&1)<<4) + (x&15)};
            expected[offset]=(x%4==0 || x%4==3) ? 255 : 0;
        }
        for (size_t i{};i<expected.size();++i) {
            if (host[i]!=expected[i]) {
                std::fprintf(stderr,"BRIDGE INIT mismatch %zu got=%u expected=%u\n",i,unsigned(host[i]),unsigned(expected[i]));
                std::abort();
            }
        }
        // Buffer sub-sector write must invalidate exactly the image cells.
        op.Fill(buffer->GetView(16,16),0x88776655); op.Submit();
        assert(image->texture->regionStale.load());
        op.Read(image.get()); op.Submit();
        auto staging{gpu.memory.AllocateStagingBuffer(image->texture->surfaceSize)};
        if (image->texture->cycle) image->texture->cycle->WaitSubmit();
        auto copy{gpu.scheduler.Submit([&](auto &commands) {
            image->texture->CopyIntoStagingBuffer(commands,staging);
        })};
        copy->AttachObjects(staging,image->texture); copy->Wait(true);
        for (size_t y{};y<12;++y) for (size_t x{};x<80;++x) {
            const auto got{staging->data()[y*80+x]};
            const u8 want{y==0 && x>=16 && x<32 ?
                std::array<u8,4>{0x55,0x66,0x77,0x88}[x%4] :
                ((x%4==0 || x%4==3) ? u8{255} : u8{0})};
            if (got!=want) {
                std::fprintf(stderr,"BRIDGE BUFFER->IMAGE mismatch (%zu,%zu) got=%u expected=%u\n",x,y,unsigned(got),unsigned(want));
                std::abort();
            }
        }
        // Future writes through the retained image view must invalidate the
        // already registered buffer, not just work at initial admission.
        op.Clear(image.get(),{0,1,0,1}); op.Submit();
        op.Read(view); op.Submit(); buffer->WaitOnFence();
        std::fill(expected.begin(),expected.end(),0x2D);
        for (size_t y{};y<12;++y) for (size_t x{};x<80;++x) {
            const size_t offset{((y/8)*2+x/64)*512 + ((y&6)<<5) +
                ((x&32)<<3) + ((x&16)<<1) + ((y&1)<<4) + (x&15)};
            expected[offset]=(x%4==1 || x%4==3) ? 255 : 0;
        }
        for (size_t i{};i<expected.size();++i) assert(buffer->backing->data()[i]==expected[i]);
        // CPU guest publication must retain the newest image cells and leave
        // all unencoded padding/exclusive buffer bytes intact.
        std::vector<u8> cpu(4096);
        op.Run([&] { buffer->Read(true,[]{ assert(false); },cpu,0); }); op.Submit();
        assert(std::equal(cpu.begin(),cpu.end(),expected.begin()));
        assert(std::equal(bytes+4096,bytes+8192,expected.begin()));
        gpu.scheduler.Complete();
        std::puts("PASS: real manager-driven staged guest bridge 20x12, edge bytes, Buffer->Image->Buffer, CPU publication");
    }

    void CheckMixedAdmission(GPU &gpu) {
        constexpr size_t size{6*4096};
        for (unsigned kind{};kind<4;++kind) {
            auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
            assert(bytes!=MAP_FAILED);
            std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
            std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
            auto descriptor=PartialDescriptor(bytes+4096);
            if (kind==0) descriptor.format=format::B8G8R8A8Unorm; // Other 4-byte formats stay rejected; RGBA8 sRGB is admitted.
            if (kind==1) descriptor.dimensions={20,12,1}; // Non-dense/partial GOB footprint.
            if (kind==2) descriptor.mappingsResident=false;
            std::shared_ptr<TextureView> image;
            BufferView view; bool rejected{};
            try {
                image=gpu.texture.FindOrCreate(descriptor);
                view=gpu.buffer.FindOrCreate({bytes+8192,8192});
                // Admission-only DMI control: a staged Vulkan allocation avoids
                // pretending the host supports Android imported-memory transport.
                if (kind==3) view.GetBuffer()->isDirect=true;
                operation.Read(view);
            } catch (const std::runtime_error &) { rejected=true; }
            if (kind==3 && view) view.GetBuffer()->isDirect=false;
            assert(rejected && operation.executor.slot->nodes.empty());
            if (image) assert(!image->texture->regionAuthority.load() && !image->texture->gpuWriteSerial);
            if (view) assert(!view.GetBuffer()->regionAuthority.load() && !view.GetBuffer()->gpuWriteSerial);
            assert(std::all_of(bytes,bytes+size,[](u8 value) { return value==0x2D; }));
        }
        std::puts("PASS: unsupported BGRA8/partial-GOB/nonresident/DMI-registration classes reject before writer/node publication (DMI transport not tested)");
    }

    void CheckMixedBufferCpuApis(GPU &gpu) {
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        auto image=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096)); operation.Read(image.get()); operation.Clear(image.get(),{1,0,0,1}); operation.Submit();
        auto view=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto buffer=view.GetBuffer()->shared_from_this(); operation.Read(view); operation.Submit();
        span<u8> readOnly;
        operation.Run([&] { operation.executor.AttachBuffer(view); readOnly=buffer->GetReadOnlyBackingSpan(true,[]{ assert(false); }); }); operation.Submit();
        const std::array<u8,4> red{255,0,0,255};
        for (size_t i{};i<8192;++i) assert(readOnly[i]==(i<4096 ? red[i%4] : 0x2D));
        buffer->WaitOnFence();
        std::vector<u8> data(16); std::iota(data.begin(),data.end(),0x71);
        operation.Run([&] { operation.executor.AttachBuffer(view); assert(!buffer->Write(data,16,operation.executor.usageTracker)); }); operation.Submit();
        operation.Read(image.get()); operation.Submit();
        auto encoded=DownloadPartial(image->texture);
        for (size_t i{};i<8192;++i) assert(encoded[i]==(i>=4112 && i<4128 ? data[i-4112] : red[i%4]));
        // CopyFrom's source mirror is stale after this new image producer. Its
        // real CPU path must choose the supplied GPU copy and defer authority to
        // the actual executor publication, including buffer-exclusive bytes.
        operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        auto destination=buffer->GetView(4096,16); unsigned callbacks{};
        operation.Run([&] {
            operation.executor.AttachBuffer(view);
            buffer->CopyFrom(4096,buffer.get(),0,16,operation.executor.usageTracker,[&] {
                ++callbacks;
                operation.executor.AddOutsideRpCommand([buffer](auto &cb,const auto &,auto &) {
                    cb.copyBuffer(buffer->GetBacking(),buffer->GetBacking(),vk::BufferCopy{.srcOffset=0,.dstOffset=4096,.size=16});
                },{},span<BufferView>{destination});
            });
        }); operation.Submit();
        assert(callbacks==1); buffer->WaitOnFence();
        const std::array<u8,4> green{0,255,0,255};
        for (size_t i{};i<8192;++i) assert(buffer->backing->data()[i]==(i<4112 ? green[i%4] : 0x2D));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: exact Buffer::GetReadOnlyBackingSpan, Write and mirror-stale CopyFrom use current physical bytes and publish precise destination cells");
    }

    void CheckMixedCoalesceReview(GPU &gpu, bool attached) {
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        *gpu.state.settings->freeGuestTextureMemory=false;
        auto image=gpu.texture.FindOrCreate(PartialDescriptor(bytes+4096)); operation.Read(image.get()); operation.Clear(image.get(),{1,0,0,1}); operation.Submit();
        auto retained=gpu.buffer.FindOrCreate({bytes+8192,8192}); auto old=retained.GetBuffer()->shared_from_this(); operation.Read(retained);
        operation.Fill(old->GetView(4096,4096),0xB4B4B4B4); operation.Submit();
        operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        const std::array<u8,4> green{0,255,0,255};
        BufferView joined;
        if (attached) {
            // Keep a real current-operation consumer and the source resource
            // lock alive while manager coalescing calls actual AttachLockedBuffer.
            operation.Read(retained);
            auto readback=gpu.memory.AllocateStagingBuffer(8192);
            operation.Run([&] {
                operation.executor.AttachBuffer(retained);
                operation.executor.AddOutsideRpCommand([old,readback](auto &cb,const auto &,auto &) {
                    cb.copyBuffer(old->GetBacking(),readback->vkBuffer,vk::BufferCopy{.size=8192});
                });
            });
            assert(old->tag.load()==operation.executor.tag && !operation.executor.slot->nodes.empty());
            const auto originalGroup=old->regionAuthority.load();
            const auto originalNodes=operation.executor.slot->nodes.size();
            const auto originalMappings=gpu.buffer.bufferMappings;
            const std::vector<u8> originalGuest(bytes,bytes+size);
            bool allocationRejected{};
            try {
                gpu.buffer.FindOrCreate({bytes,4*4096},operation.executor.tag,[](auto,auto &&) { throw std::bad_alloc{}; });
            } catch (const std::bad_alloc &) { allocationRejected=true; }
            assert(allocationRejected && retained.GetBuffer()==old.get() && old->regionTracked && old->regionAuthority.load()==originalGroup);
            assert(gpu.buffer.bufferMappings==originalMappings && operation.executor.slot->nodes.size()==originalNodes);
            assert(std::equal(originalGuest.begin(),originalGuest.end(),bytes));
            operation.Run([&] {
                try {
                    joined=gpu.buffer.FindOrCreate({bytes,4*4096},operation.executor.tag,[&](auto buffer,auto &&lock) {
                        operation.executor.AttachLockedBuffer(std::move(buffer),std::move(lock));
                    });
                } catch (const TextureAliasReconciliationRequest &) {
                    // Resolve only delegate objects, never dereference a possibly
                    // destroyed Buffer. The manager or executor must retain the
                    // published replacement, or the original must be intact.
                    auto raw=retained.GetBuffer();
                    const bool managerOwns=std::any_of(gpu.buffer.bufferMappings.begin(),gpu.buffer.bufferMappings.end(),[&](const auto &buffer) { return buffer.get()==raw; });
                    const bool executorOwns=std::any_of(operation.executor.attachedBuffers.begin(),operation.executor.attachedBuffers.end(),[&](const auto &buffer) { return buffer.get()==raw; });
                    const bool originalIntact=raw==old.get() && old->regionTracked;
                    std::fprintf(stderr,"ATTACHED COALESCE RETRY: originalIntact=%d managerOwnsDelegateTarget=%d executorOwnsDelegateTarget=%d\n",originalIntact,managerOwns,executorOwns);
                    assert(originalIntact || managerOwns || executorOwns);
                    throw;
                }
            });
            operation.Submit(); gpu.scheduler.Complete();
            for (size_t i{};i<8192;++i) assert(readback->data()[i]==(i<4096 ? green[i%4] : 0xB4));
        } else {
            // The foreign image publishes its newer bytes while the source raw
            // buffer remains stale there and GPU-dirty only for exclusive bytes.
            { std::scoped_lock resource{*image->texture}; image->texture->SynchronizeGuest(false,true); }
            assert(old->dirtyState==Buffer::DirtyState::GpuDirty && old->regionStale);
            for (size_t i{};i<4096;++i) assert(bytes[8192+i]==green[i%4]);
            joined=gpu.buffer.FindOrCreate({bytes,4*4096});
            size_t stale{};
            for (size_t i{};i<4096;++i) stale+=bytes[8192+i]!=green[i%4];
            std::fprintf(stderr,"PUBLISHED FOREIGN COALESCE: %zu / 4096 guest bytes corrupted\n",stale);
            assert(stale==0);
        }
        assert(joined && retained.GetBuffer()==joined.GetBuffer());
        operation.Read(joined); operation.Submit();
        std::vector<u8> cpu(4*4096);
        operation.Run([&] { joined.GetBuffer()->Read(true,[]{ assert(false); },cpu,0); }); operation.Submit();
        for (size_t i{};i<cpu.size();++i) assert(cpu[i]==(i>=4096 && i<3*4096 ? green[i%4] : i>=3*4096 ? 0xB4 : 0x2D));
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS: reviewed mixed coalesce %s, retained delegates/consumers and actual guest bytes\n",attached?"attached-source retry lifetime":"already-published foreign cells");
    }

    void CheckMixedRelevantPublication(GPU &gpu) {
        constexpr size_t size{6*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        *gpu.state.settings->freeGuestTextureMemory=false;
        auto first=gpu.texture.FindOrCreate(PartialDescriptor(bytes)); operation.Read(first.get()); operation.Clear(first.get(),{1,0,0,1}); operation.Submit();
        auto target=gpu.buffer.FindOrCreate({bytes,4096}); operation.Read(target); operation.Submit();
        auto bridge=gpu.buffer.FindOrCreate({bytes+4096,8192}); operation.Read(bridge); operation.Submit();
        std::shared_ptr<TextureView> unrelated;
        operation.Run([&] { unrelated=gpu.texture.FindOrCreate(PartialDescriptor(bytes+8192),operation.executor.tag); });
        operation.Read(unrelated.get()); operation.Clear(unrelated.get(),{0,0,1,1}); operation.Submit();
        assert(first->texture->regionAuthority.load()==unrelated->texture->regionAuthority.load() && target.GetBuffer()!=bridge.GetBuffer());
        // I2 owns no byte of B1. Its latest producer cycle must remain pending
        // from the CPU reader's perspective; publication of B1 needs only I1.
        const auto unrelatedCycle=unrelated->texture->cycle;
        const auto waits=unrelatedCycle->gpuWaits;
        const auto readbacks=unrelated->texture->gpuReadbackSerial;
        std::vector<u8> cpu(4096);
        operation.Run([&] { target.GetBuffer()->Read(true,[]{ assert(false); },cpu,0); }); operation.Submit();
        const std::array<u8,4> red{255,0,0,255};
        for (size_t i{};i<cpu.size();++i) assert(cpu[i]==red[i%4]);
        const auto extraWaits=unrelatedCycle->gpuWaits-waits;
        const bool extraReadback=unrelated->texture->gpuReadbackSerial!=readbacks;
        std::fprintf(stderr,"RELEVANT PUBLICATION: unrelated image extra GPU waits=%u readback=%d\n",extraWaits,extraReadback);
        assert(extraWaits==0 && !extraReadback);
        for (size_t i=8192;i<16384;++i) assert(bytes[i]==0x2D);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: CPU buffer publication touches only relevant producers within a connected mixed chain");
    }

    void CheckMixedDrain(GPU &gpu) {
        constexpr size_t size{4*4096};
        auto *bytes=static_cast<u8 *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        assert(bytes!=MAP_FAILED);
        std::unique_ptr<u8,std::function<void(u8 *)>> memory(bytes,[](auto *p) { munmap(p,size); });
        std::fill(bytes,bytes+size,0x2D); MixedManagerCleanup cleanup{gpu}; MixedExecutor operation{gpu};
        *gpu.state.settings->freeGuestTextureMemory=false;
        auto image=gpu.texture.FindOrCreate(PartialDescriptor(bytes));
        operation.Read(image.get()); operation.Clear(image.get(),{0,1,0,1}); operation.Submit();
        auto first=gpu.buffer.FindOrCreate({bytes,4096}); operation.Read(first); operation.Submit();
        auto second=gpu.buffer.FindOrCreate({bytes+4096,8192}); operation.Read(second); operation.Submit();
        operation.Fill(first,0x3A3A3A3A); operation.Submit();
        operation.Fill(second.SubView(4096,4096),0x6D6D6D6D); operation.Submit();
        assert(first.GetBuffer()!=second.GetBuffer());
        assert(first.GetBuffer()->regionAuthority.load()==second.GetBuffer()->regionAuthority.load());
        Texture::DrainRegionAuthority({image->texture},0);
        assert(!first.GetBuffer()->regionTracked && !second.GetBuffer()->regionTracked && !image->texture->regionTracked);
        const std::array<u8,4> green{0,255,0,255};
        for(size_t i{};i<size;++i) {
            const auto expected=i<4096?0x3A:i<8192?green[i%4]:i<12288?0x6D:0x2D;
            assert(bytes[i]==expected);
        }
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: mixed drain publishes every independent buffer writer, image cells and exclusive bytes before detachment");
    }
