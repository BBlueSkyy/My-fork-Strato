void ClearDepthAlias(const std::shared_ptr<Texture> &texture, float depth, u32 stencil) {
    AliasRead(texture);
    std::scoped_lock lock{*texture};
    auto previous{texture->cycle};
    if (previous) previous->WaitSubmit();
    texture->cycle = texture->gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
        const vk::ImageSubresourceRange range{texture->format->vkAspect,0,1,0,texture->layerCount};
        commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},
            vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask=vk::AccessFlagBits::eTransferWrite,.oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=texture->GetBacking(),.subresourceRange=range});
        commands.clearDepthStencilImage(texture->GetBacking(),vk::ImageLayout::eGeneral,{depth,stencil},range);
    });
    texture->cycle->AttachObject(texture);
    texture->cycle->ChainCycle(previous);
    texture->RecordGpuWrite();
    texture->SynchronizeHost(true);
}
void VerifyDepthAlias(const std::shared_ptr<Texture> &texture, const std::vector<u8> &encoded) {
    AliasRead(texture);
    std::scoped_lock lock{*texture};
    const auto size{texture::GetDepthStencilTransferSize(texture->surfaceSize,texture->mipLayouts,texture->layerCount,4)};
    auto readback{texture->gpu.memory.AllocateStagingBuffer(size)};
    texture->WaitOnFence();
    auto cycle{texture->gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {texture->CopyIntoStagingBuffer(commands,readback);})};
    cycle->Wait();
    texture::PackDepthStencil({readback->data(),size},texture->format->stencilFirst ? texture::DepthStencilPacking::Stencil8Depth24 :
        texture::DepthStencilPacking::Depth24Stencil8,texture->mipLayouts,texture->layerCount,texture->surfaceSize);
    const auto expected{AliasHostBytes(texture,encoded)};
    assert(std::equal(expected.begin(),expected.end(),readback->data()));
}
void CheckDepthStencilAliases(GPU &gpu) {
    const auto aspects{vk::ImageAspectFlagBits::eDepth|vk::ImageAspectFlagBits::eStencil};
    NativeFormat first{vk::Format::eD24UnormS8Uint,aspects,1,1,4,false};
    NativeFormat second{vk::Format::eD24UnormS8Uint,aspects,1,1,4,true};
    assert(first.IsCompatible(first));
    assert(!first.IsCompatible(second));
    assert(!second.IsCompatible(first));
    TextureManager manager{gpu};
    std::vector<u8> bytes(1024,0), expected{bytes};
    GuestTexture descriptor;
    descriptor.format=&first; descriptor.dimensions=vk::Extent3D{16,8,1}; descriptor.viewType=vk::ImageViewType::e2DArray;
    descriptor.aspect=aspects; descriptor.mipLevelCount=descriptor.viewMipCount=1;
    descriptor.layerCount=2; descriptor.layerStride=512; descriptor.mappings.push_back(span<u8>{bytes});
    assert(Texture::CanMaterializeGuestLayout(descriptor));
    assert(Texture::IsGuestLayoutReversible(descriptor));
    auto excluded{descriptor}; NativeFormat padded{vk::Format::eD32SfloatS8Uint,aspects,1,1,8,false};
    excluded.format=&padded; assert(!Texture::CanMaterializeGuestLayout(excluded));
    excluded=descriptor; excluded.tileConfig.mode=texture::TileMode::Pitch; assert(!Texture::CanMaterializeGuestLayout(excluded));
    excluded=descriptor; excluded.dimensions.depth=2; assert(!Texture::CanMaterializeGuestLayout(excluded));
    auto a{manager.FindOrCreate(descriptor)};
    AliasRead(a->texture);
    const auto cachedSubmissions{gpu.scheduler.submissions};
    assert(manager.FindOrCreate(descriptor)->texture==a->texture);
    assert(gpu.scheduler.submissions==cachedSubmissions);
    ClearDepthAlias(a->texture,1.0f,0x37);
    EncodeAliasBase(a->texture,expected,{0xff,0xff,0xff,0x37});
    auto alternate{descriptor}; alternate.format=&second;
    std::shared_ptr<TextureView> b;
    interconnect::CommandExecutor executor; executor.pending=a->texture;
    unsigned registrations{};
    texture::RetryBackingPromotion([&] {b=manager.FindOrCreate(alternate,1);},[&](const auto &request) {
        using Request=std::remove_cvref_t<decltype(request)>;
        if constexpr(std::is_same_v<Request,TextureAliasReconciliationRequest>) {
            ++registrations; assert(!request.guestMemoryLeases.empty());
            assert(a->texture->guestMemoryPins);
            *gpu.state.settings->freeGuestTextureMemory=true;
            a->texture->FreeGuest();
            assert(!a->texture->memoryFreed);
            *gpu.state.settings->freeGuestTextureMemory=false;
            executor.HandleTextureBackingPromotion(request);
        } else assert(false);
    });
    assert(registrations==1 && b->texture!=a->texture && bytes==expected);
    assert(a->texture->aliasGroup.load()==b->texture->aliasGroup.load());
    assert(!a->texture->guestMemoryPins && !b->texture->guestMemoryPins);
    TextureView *writers[]{a.get(),b.get()};
    bool conflict{};
    try { Texture::ValidateAliasWrites(writers); } catch(const std::runtime_error &) {conflict=true;}
    assert(conflict);
    VerifyDepthAlias(b->texture,expected);
    ClearDepthAlias(b->texture,0.0f,0xA1);
    EncodeAliasBase(b->texture,expected,{0xA1,0,0,0});
    assert(a->texture->aliasStale);
    bool staleGuard{};
    try {a->texture->RequireAliasCoherence(1);} catch(const TextureAliasReconciliationRequest &) {staleGuard=true;}
    assert(staleGuard);
    VerifyDepthAlias(a->texture,expected);
    assert(bytes==expected);
    ClearDepthAlias(a->texture,1.0f,0x72);
    EncodeAliasBase(a->texture,expected,{0xff,0xff,0xff,0x72});
    VerifyDepthAlias(b->texture,expected);
    AliasRead(a->texture);
    const auto settled{gpu.scheduler.submissions};
    const auto allocations{gpu.memory.count};
    for(unsigned i{};i<4;++i) {
        AliasRead(a->texture); AliasRead(b->texture);
        assert(manager.FindOrCreate(descriptor)->texture==a->texture);
        assert(manager.FindOrCreate(alternate)->texture==b->texture);
    }
    assert(gpu.scheduler.submissions==settled && gpu.memory.count==allocations);
}
