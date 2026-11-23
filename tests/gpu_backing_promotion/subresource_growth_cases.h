// SPDX-License-Identifier: MPL-2.0
// Audit-only: explicit known-region copies are controls, not a production authority policy.
namespace skyline::gpu {
    void CheckSubresourceGrowth(GPU &gpu, bool freeGuest, bool partial) {
        *gpu.state.settings->freeGuestTextureMemory = freeGuest;
        constexpr size_t stride=180224;
        std::vector<u8> guest(stride*6,0);
        auto parent=std::make_shared<Texture>(gpu,vk::Format::eR16G16B16A16Sfloat,vk::Extent3D{128,128,1},1,6);
        parent->guest->layerStride=stride; parent->mirror=guest;
        parent->guest->mappings.push_back(parent->mirror);
        parent->dirtyState=Texture::CpuDirty;
        // Pin models the production growth lease: ON must not discard the input bytes.
        auto pin=parent->PinGuestMemory();
        parent->SynchronizeHost(false);
        TextureView cachedBase{parent,vk::ImageViewType::e2DArray,
            {vk::ImageAspectFlagBits::eColor,0,1,0,6},parent->format,{}};
        const auto oldView=cachedBase.GetView();
        const auto layouts=texture::GetBlockLinearMipLayout(parent->dimensions,1,1,8,1,1,8,1,1,8);
        std::vector<std::shared_ptr<Texture>> children;
        std::vector<std::shared_ptr<memory::StagingBuffer>> uploads;
        std::vector<vk::ImageCopy> regions;
        size_t guestOffset=layouts[0].blockLinearSize;
        for(u32 mip=1;mip<8;++mip) {
            const auto &level=layouts[mip];
            auto child=std::make_shared<Texture>(gpu,vk::Format::eR16G16B16A16Sfloat,level.dimensions,1,6);
            child->guest->layerStride=stride;
            child->mirror={guest.data()+guestOffset,guest.size()-guestOffset};
            child->guest->mappings.push_back(child->mirror);
            child->dirtyState=Texture::CpuDirty;
            ++child->guestMemoryPins;
            child->SynchronizeHost(false);
            auto upload=gpu.memory.AllocateStagingBuffer(level.linearSize*6);
            // Six independently distinguishable RGBA16F payloads, known by construction.
            for(u32 face=0;face<6;++face) {
                const std::array<uint16_t,4> pixel{uint16_t(0x3c00+face),uint16_t(0x3800+mip),0,0x3c00};
                for(size_t off=0;off<level.linearSize;off+=8)
                    std::memcpy(upload->data()+face*level.linearSize+off,pixel.data(),8);
            }
            const auto extent=partial ? vk::Extent3D{1,1,1} : level.dimensions;
            std::vector<vk::BufferImageCopy> writes;
            for(u32 face=0;face<6;++face)
                writes.push_back({.bufferOffset=face*level.linearSize,
                    .imageSubresource={vk::ImageAspectFlagBits::eColor,0,face,1},.imageExtent=extent});
            child->RecordGpuWrite(); child->dirtyState=Texture::GpuDirty;
            child->cycle=gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
                const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,6};
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},
                    vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                        .image=child->GetBacking(),.subresourceRange=range});
                commands.copyBufferToImage(upload->vkBuffer,child->GetBacking(),vk::ImageLayout::eGeneral,writes);
            });
            child->cycle->AttachObjects(child,upload);
            // Leave the last producer submitted but not CPU-waited through expansion.
            if(mip<7) child->cycle->Wait();
            children.push_back(child); uploads.push_back(upload);
            regions.push_back({.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,6},
                .dstSubresource={vk::ImageAspectFlagBits::eColor,mip,0,6},.extent=extent});
            guestOffset+=level.blockLinearSize;
        }
        assert(std::all_of(guest.begin(),guest.end(),[](u8 x){return x==0;}));
        // Oracle CPU update after child upload: another pixel is newer in guest.
        // This is a fixture boundary, not a test of Android trapping callbacks.
        if(partial) guest[layouts[0].blockLinearSize+9]=0x3c;
        parent->PromoteBackingForView(parent->format->vkFormat,8);
        assert(cachedBase.GetView()!=oldView);
        assert(parent->guest->mappings.front().data()==guest.data());
        auto before=gpu.memory.AllocateStagingBuffer(parent->surfaceSize);
        auto read=[&](const auto &buffer) {
            auto cycle=gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands){parent->CopyIntoStagingBuffer(commands,buffer);});
            cycle->AttachObjects(parent,buffer);cycle->Wait(true);
        };
        read(before);
        if(partial) {
            const auto cpuOffset=layouts[0].linearSize*6+9;
            assert(before->data()[cpuOffset]==0x3c);
            before->data()[cpuOffset]=0;
        }
        assert(std::all_of(before->data(),before->data()+parent->surfaceSize,[](u8 x){return x==0;}));
        // All producers actually complete now: waiting alone did not transfer child data.
        for(const auto &child:children)child->cycle->Wait();
        if(partial) {
            auto source=gpu.memory.AllocateStagingBuffer(children[0]->surfaceSize);
            auto c=gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands){children[0]->CopyIntoStagingBuffer(commands,source);});
            c->AttachObjects(source,children[0]);c->Wait();
            assert(source->data()[9]==0); // Full-child copy loses the newer guest pixel.
        }
        // Positive control with oracle-known regions only. Not integrated into renderer.
        auto copied=gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands){
            for(size_t i=0;i<children.size();++i) {
                const vk::ImageSubresourceRange src{vk::ImageAspectFlagBits::eColor,0,1,0,6};
                const vk::ImageSubresourceRange dst{vk::ImageAspectFlagBits::eColor,u32(i+1),1,0,6};
                std::array<vk::ImageMemoryBarrier,2> barriers{};
                for(auto &b:barriers){b.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite;
                    b.oldLayout=b.newLayout=vk::ImageLayout::eGeneral;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;}
                barriers[0].image=children[i]->GetBacking();barriers[0].subresourceRange=src;barriers[0].dstAccessMask=vk::AccessFlagBits::eTransferRead;
                barriers[1].image=parent->GetBacking();barriers[1].subresourceRange=dst;barriers[1].dstAccessMask=vk::AccessFlagBits::eTransferWrite;
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},barriers);
                commands.copyImage(children[i]->GetBacking(),vk::ImageLayout::eGeneral,parent->GetBacking(),vk::ImageLayout::eGeneral,regions[i]);
            }
        });
        for(const auto &child:children)copied->AttachObject(child);
        copied->AttachObject(parent);copied->Wait();
        auto after=gpu.memory.AllocateStagingBuffer(parent->surfaceSize); read(after);
        size_t offset=layouts[0].linearSize*6;
        for(u32 mip=1;mip<8;++mip) {
            const auto &level=layouts[mip];
            for(u32 face=0;face<6;++face) {
                const auto *actual=after->data()+offset+face*level.linearSize;
                if(partial){assert(!std::memcmp(actual,uploads[mip-1]->data()+face*level.linearSize,8));
                    if(mip==1 && face==0) {
                        assert(actual[9]==0x3c);
                        // Whole-child copy would overwrite this newer CPU byte.
                        assert(std::all_of(actual+10,actual+level.linearSize,[](u8 x){return x==0;}));
                    } else assert(std::all_of(actual+8,actual+level.linearSize,[](u8 x){return x==0;}));}
                else assert(!std::memcmp(actual,uploads[mip-1]->data()+face*level.linearSize,level.linearSize));
            }
            offset+=level.linearSize*6;
        }
        assert(std::all_of(after->data(),after->data()+layouts[0].linearSize*6,[](u8 x){return x==0;}));
        std::printf("OBSERVED mip-growth free=%u partial=%u missing-child-subresources=42 explicit-region-copy-control=correct\n",unsigned(freeGuest),unsigned(partial));
        gpu.scheduler.Complete();
        *gpu.state.settings->freeGuestTextureMemory=false;
    }
}
