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
        Buffer seed{gpu,raw.size()}, producer{gpu,raw.size()}, result{gpu,raw.size()};
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
        Buffer second{gpu,raw.size()};
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
