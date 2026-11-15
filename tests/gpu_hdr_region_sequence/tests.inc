// SPDX-License-Identifier: MPL-2.0
// Test-only HDR transport/oracle on PR400's existing real Vulkan fixture.
// Logged physical spans are exact; omitted secondary TIC geometries are synthetic.
    struct HdrRegionFixture {
        struct Image { GuestTexture guest;std::shared_ptr<Texture> host;std::shared_ptr<TextureView> view;size_t base; };
        GPU &gpu;NativeFormat hdr{vk::Format::eB10G11R11UfloatPack32};
        std::vector<u8> bytes,oracle;std::vector<Image> images;
        explicit HdrRegionFixture(GPU &value,size_t size):gpu{value},bytes(size,0x2d),oracle(bytes){if(std::getenv("TEXMAN_HDR_RGBA8_CONTROL"))hdr.vkFormat=vk::Format::eR8G8B8A8Unorm;}
        size_t Add(size_t base,vk::Extent3D extent,u32 levels,u32 bh,size_t expectedSize=0){
            GuestTexture d;d.format=&hdr;d.dimensions=extent;d.viewType=vk::ImageViewType::e2D;
            d.layerCount=d.backingLayerCount=d.viewLayerCount=1;d.mipLevelCount=d.viewMipCount=levels;
            d.tileConfig.blockHeight=bh;d.tileConfig.blockDepth=1;d.tileConfig.tileWidthSpacing=0;
            d.layerStride=d.CalculateLayerSize();if(expectedSize)assert(d.layerStride==expectedSize);
            assert(base<=bytes.size()&&d.layerStride<=bytes.size()-base);d.mappings={{bytes.data()+base,d.layerStride}};
            auto host{std::make_shared<Texture>(gpu,d)};host->SetupGuestMappings();host->TransitionLayout(vk::ImageLayout::eGeneral);host->SynchronizeHost(false);
            auto view{host->GetView(vk::ImageViewType::e2D,{vk::ImageAspectFlagBits::eColor,0,levels,0,1},&hdr,{},0)};
            images.push_back({d,host,view,base});return images.size()-1;
        }
        std::vector<std::shared_ptr<Texture>> Members(){std::vector<std::shared_ptr<Texture>> result;for(auto &i:images)if(i.host)result.push_back(i.host);return result;}
        std::shared_ptr<memory::StagingBuffer> Read(size_t index,bool repair=true){
            auto &i{images[index]};if(repair)AliasRead(i.host);auto out{gpu.memory.AllocateStagingBuffer(i.host->surfaceSize)};
            if(i.host->cycle)i.host->cycle->WaitSubmit();auto cycle{gpu.scheduler.Submit([&](auto &cmd){i.host->CopyIntoStagingBuffer(cmd,out);})};
            cycle->AttachObjects(i.host,out);cycle->Wait(true);return out;
        }
        size_t Mismatches(size_t index,bool repair=true){
            auto out{Read(index,repair)};auto &i{images[index]};size_t bad{};
            VisitRegionBytes(i.guest,[&](size_t physical,size_t linear){if(out->data()[linear]!=oracle[i.base+physical]){
                if(bad<3)std::printf("HDR_MISMATCH image=%zu dims=%ux%u mips=%u physical=%zu linear=%zu expected=%02x actual=%02x\n",index,i.guest.dimensions.width,i.guest.dimensions.height,i.guest.mipLevelCount,physical,linear,oracle[i.base+physical],out->data()[linear]);++bad;}});std::fflush(stdout);return bad;
        }
        // Real buffer->image GPU write with a nonuniform raw packed-float pattern.
        // Exact format bits include finite values, subnormals, infinities and NaNs.
        void Paint(size_t index,u32 seed,int mip=-1,ContextTag owner={},std::function<void()> beforeSubmit={}){
            auto &i{images[index]};AliasRead(i.host);ContextLock<Texture> resource{owner,*i.host};
            auto data{gpu.memory.AllocateStagingBuffer(i.host->surfaceSize)};auto copies{i.host->GetBufferImageCopies()};
            size_t first{},last{i.host->surfaceSize};
            if(mip>=0){for(int l{};l<mip;++l)first+=i.host->mipLayouts[l].linearSize;last=first+i.host->mipLayouts[mip].linearSize;
                copies.erase(std::remove_if(copies.begin(),copies.end(),[&](const auto& c){return c.imageSubresource.mipLevel!=static_cast<u32>(mip);}),copies.end());}
            VisitRegionBytes(i.guest,[&](size_t physical,size_t linear){if(linear<first||linear>=last)return;
                static constexpr u32 special[]{0x00000000,0x00000001,0x7bffffff,0xf83e07c0,0xf87e0fc1,0xffffffff};
                const u32 word{(linear/4)%13<6?special[(linear/4)%13]:(seed*0x9e3779b9u+static_cast<u32>(linear/4)*0x45d9f3bu)};
                const auto value{static_cast<u8>(word>>(8*(linear%4)))};data->data()[linear]=value;oracle[i.base+physical]=value;});
            i.host->RecordGpuWrite();i.host->SynchronizeHost(true);auto previous{i.host->cycle};if(previous)previous->WaitSubmit();
            auto cycle{gpu.scheduler.Submit([&](auto &cmd){
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},
                    vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,.dstAccessMask=vk::AccessFlagBits::eTransferWrite},{},{});
                cmd.copyBufferToImage(data->vkBuffer,i.host->GetBacking(),vk::ImageLayout::eGeneral,copies);
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,{},
                    vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,.dstAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite},{},{});
                if(beforeSubmit)beforeSubmit();
            })};cycle->AttachObjects(i.host,data);cycle->ChainCycle(previous);i.host->cycle=cycle;
        }
        void Check(){for(size_t i{};i<images.size();++i)if(images[i].host)assert(Mismatches(i)==0);}
        ~HdrRegionFixture(){gpu.scheduler.Complete();}
    };

    void CheckHdrLoggedRegionLimit(GPU &gpu){
        assert(!texture::MixedRegionAuthorityEnabled);*gpu.state.settings->freeGuestTextureMemory=false;
        HdrRegionFixture f{gpu,16777216};constexpr size_t root{0xcb0000};
        assert(root==uintptr_t{0x86bb4d000ULL}-uintptr_t{0x86ae9d000ULL});
        f.Add(root,{640,360,1},1,16,983040); // child
        f.Add(root,{640,360,1},2,16,1310720); // requested parent
        f.Add(root+983040,{320,180,1},1,16,327680);
        f.Add(root+1048576,{320,180,1},5,16,438272);
        f.Add(root+1048576,{320,256,1},1,16,327680); // explicit unknown-TIC hypothesis
        f.Add(root-0x80000,{640,720,1},1,16,1966080);
        f.Add(root-0x100000,{1280,720,1},1,16,3932160);
        f.Add(root-0x100000,{1280,768,1},1,16,3932160);
        f.Add(0,{2048,2048,1},1,16,16777216);
        f.Add(root,{20,11,1},1,2,2048);f.Add(root,{40,22,1},1,4,6144);
        f.Add(root,{80,45,1},1,8,20480);f.Add(root,{160,90,1},1,16,81920);
        assert(Texture::PhysicalImageSpans(f.images[0].guest,true).size()==1800);
        assert(Texture::PhysicalImageSpans(f.images[1].guest,true).empty()); // mip1 partial edge
        assert(Texture::PhysicalImageSpans(f.images[2].guest,true).empty());
        assert(Texture::PhysicalImageSpans(f.images[3].guest,true).empty());
        assert(Texture::PhysicalImageSpans(f.images[4].guest,true).size()==640);
        assert(Texture::PhysicalImageSpans(f.images[6].guest,true).empty()); // >4096 full GOBs
        assert(Texture::PhysicalImageSpans(f.images[8].guest).size()==1); // dense ROB path remains possible in isolation
        assert(Texture::PhysicalImageSpans(f.images[8].guest,true).empty()); // forced fine partition exceeds budget
        const auto count{gpu.memory.count};auto members{f.Members()};
        assert(!Texture::RegisterRegionAuthority(members,0));
        for(auto &i:f.images)assert(!i.host->regionTracked && !i.host->regionAuthority.load());
        assert(gpu.memory.count==count);
        // Admission refused, so later retained readers cannot become coherent by
        // calling the normal image-only preflight on these unsupported layouts.
        f.Paint(1,17);f.Paint(2,31);f.Paint(3,47,0);
        const auto staleParent{f.Mismatches(1)},staleTail{f.Mismatches(2)};
        assert(staleParent && staleTail);assert(f.Mismatches(3)==0);
        std::printf("HDR_REGIONS exact_spans=13 image_only_admitted=0 mixed_gate=0 stale_parent_bytes=%zu stale_tail_bytes=%zu validation_errors=%u\n",staleParent,staleTail,validationErrors);std::fflush(stdout);
        if(std::getenv("TEXMAN_HDR_REGION_REQUIRE_ACTUAL_COHERENCE")){
            assert(!validationErrors);throw std::runtime_error("HDR_REGION_STALE_READER: retained HDR images remain stale because exact footprints cannot be admitted");}
        // Native HDR encoding is reversible for each layout's own logical pixels.
        // Padding is deliberately kept as the previous guest byte, never a texel.
        auto priorGuest{f.bytes};std::vector<bool> nativeMask(f.bytes.size());
        VisitRegionBytes(f.images[3].guest,[&](size_t p,size_t){nativeMask[f.images[3].base+p]=true;});
        f.images[3].host->SynchronizeGuest(false,true);
        for(size_t b{};b<f.bytes.size();++b)if(!nativeMask[b])assert(f.bytes[b]==priorGuest[b]);
        auto linear{f.Read(3,false)};VisitRegionBytes(f.images[3].guest,[&](size_t p,size_t l){assert(f.bytes[f.images[3].base+p]==linear->data()[l]);});
        assert(!validationErrors);
    }

    void CheckHdrAlignedRegionAuthority(GPU &gpu,bool reverse){
        assert(!texture::MixedRegionAuthorityEnabled);*gpu.state.settings->freeGuestTextureMemory=false;
        HdrRegionFixture f{gpu,2*1024*1024};constexpr size_t root{65536};
        struct Layout{size_t base;vk::Extent3D extent;u32 levels;size_t bytes;};
        const std::array<Layout,6> shapes{{{root,{512,256,1},1,524288},{root,{512,256,1},2,655360},
            {root+524288,{256,128,1},1,131072},{root+589824,{256,128,1},5,174592},
            {root+589824,{256,128,1},1,131072},{0,{512,512,1},1,1048576}}};
        std::array<size_t,6> ids{};
        for(size_t n{};n<shapes.size();++n){const size_t k{reverse?shapes.size()-1-n:n};const auto &s{shapes[k]};ids[k]=f.Add(s.base,s.extent,s.levels,16,s.bytes);}
        auto members{f.Members()};assert(Texture::RegisterRegionAuthority(members,0));
        auto group{members.front()->regionAuthority.load()};assert(group&&!group->guestBridge&&group->buffers.empty());
        std::vector<vk::Image> backings;std::vector<vk::ImageView> views;
        for(auto &i:f.images){assert(i.host->regionTracked && !i.host->regionMixed);backings.push_back(i.host->GetBacking());views.push_back(i.view->GetView());}
        f.Check(); // CPU-uploaded guest baseline is valid before the first GPU claim.
        const auto frameImages{gpu.memory.count},frameStaging{gpu.memory.stagingCount},frameSubmits{gpu.scheduler.submissions};
        const auto frameInvalidations{texture::regionInvalidationSequence.load()};
        for(unsigned frame{};frame<3;++frame){
            f.Paint(ids[0],17+frame);f.Paint(ids[2],31+frame);f.Paint(ids[3],47+frame);f.Paint(ids[5],61+frame,0);f.Check();
            // A mip0-only producer is prepared across the whole image first.
            // Its unmodified mip1 must retain the newest foreign content.
            f.Paint(ids[1],79+frame,0);f.Check();
        }
        std::printf("HDR_REGIONS active_frames=3 image_allocations=%zu staging_allocations=%zu submissions=%zu invalidation_epochs=%llu\n",
            static_cast<size_t>(gpu.memory.count-frameImages),static_cast<size_t>(gpu.memory.stagingCount-frameStaging),static_cast<size_t>(gpu.scheduler.submissions-frameSubmits),static_cast<unsigned long long>(texture::regionInvalidationSequence.load()-frameInvalidations));
        f.Paint(ids[2],103);
        auto target{f.images[ids[1]].host};const auto oldCycle{target->cycle};
        for(unsigned fault{};fault<3;++fault){
            gpu.scheduler.failRecording=fault==0;FenceCycle::failAttachment=fault==1;gpu.scheduler.failSubmit=fault==2;
            bool failed{};try{AliasRead(target);}catch(const std::exception&){failed=true;}
            gpu.scheduler.failRecording=FenceCycle::failAttachment=gpu.scheduler.failSubmit=false;
            assert(failed&&target->regionStale&&target->cycle==oldCycle);
        }
        f.Check();
        // A real native command buffer is recorded but not yet submitted here.
        // The production overlap bridge must request executor submission before
        // reading the old cycle or modifying any guest byte/version.
        bool deferred{};
        f.Paint(ids[2],113,-1,77,[&]{
            const auto guestBefore{f.bytes};const auto submissions{gpu.scheduler.submissions};
            auto writer{f.images[ids[2]].host};assert(writer->pendingGpuWriteTag.load()==77);
            try { TextureManager::SynchronizeGuestOverlaps({writer},77); }
            catch(const TextureAliasReconciliationRequest &request){
                deferred=true;assert(request.overlapReadbacks.size()==1&&request.overlapReadbacks.front()==writer);
            }
            assert(deferred&&f.bytes==guestBefore&&gpu.scheduler.submissions==submissions);
        });
        TextureManager::SynchronizeGuestOverlaps(f.Members());f.Check();
        // CPU trap publication must respect current regional owners, preserve
        // outside bytes and invalidate future uses rather than detach the link.
        auto &cpu{f.images[ids[3]]};
        // TrapManager visits the connected overlapping trap entries, not only
        // the view through which the CPU address was selected. Every current
        // regional owner must publish/relinquish before the CPU store proceeds.
        for(auto &i:f.images)gpu.state.process->trap.callbacks[*i.host->trapHandle].prepare();
        {std::scoped_lock trap{gpu.state.process->trap.mutex};for(auto &i:f.images)assert(gpu.state.process->trap.callbacks[*i.host->trapHandle].write());}
        f.bytes[cpu.base]^=0x5a;f.oracle[cpu.base]^=0x5a;f.Check();
        for(auto &i:f.images)i.host->SynchronizeGuest(false,true);
        assert(f.bytes==f.oracle); // This aligned control also checks all guard/padding bytes.
        const auto allocations{gpu.memory.count},staging{gpu.memory.stagingCount},submits{gpu.scheduler.submissions};
        const auto invalidations{texture::regionInvalidationSequence.load()};
#if defined(HDR_REGION_ALLOCATION_PROBE)
        resource_bench::allocations=0;resource_bench::countAllocations=true;
#endif
        for(unsigned n{};n<100;++n)for(auto &i:f.images)AliasRead(i.host);
#if defined(HDR_REGION_ALLOCATION_PROBE)
        resource_bench::countAllocations=false;assert(resource_bench::allocations==0);
        std::printf("HDR_REGIONS warm_host_new_allocations=%llu\n",resource_bench::allocations);
#endif
        assert(allocations==gpu.memory.count && staging==gpu.memory.stagingCount && submits==gpu.scheduler.submissions);
        assert(invalidations==texture::regionInvalidationSequence.load());
        for(size_t i{};i<f.images.size();++i)assert(f.images[i].host->GetBacking()==backings[i]&&f.images[i].view->GetView()==views[i]);
        size_t bindings{};for(auto &i:f.images)bindings+=i.host->regionBindings.size();
        std::printf("HDR_REGIONS aligned=1 reverse=%d raw_bits_exact=1 cells=%zu metadata_cell_bytes=%zu bindings=%zu binding_bytes=%zu warm_uses=600 warm_allocations=0 warm_submissions=0 validation_errors=%u\n",reverse,group->regions.size(),group->regions.size()*sizeof(texture::PhysicalImageRegion),bindings,bindings*sizeof(texture::PhysicalImageBinding),validationErrors);
        // Completed prior work may release a retained representation; a new
        // native image joins through the same image-only partition without losing
        // the current guest versions or the other members' future dependencies.
        members.clear();gpu.scheduler.Complete();const size_t removed{ids[4]};
        auto descriptor{f.images[removed].guest};const auto base{f.images[removed].base};
        std::weak_ptr<Texture> gone{f.images[removed].host};f.images[removed].view.reset();f.images[removed].host.reset();
        gpu.scheduler.Complete();assert(gone.expired());
        const auto replacement{f.Add(base,descriptor.dimensions,descriptor.mipLevelCount,descriptor.tileConfig.blockHeight,descriptor.layerStride)};
        assert(Texture::RegisterRegionAuthority(f.Members(),0));f.Paint(replacement,137);f.Check();
        assert(!validationErrors);
    }
