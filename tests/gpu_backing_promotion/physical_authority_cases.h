// SPDX-License-Identifier: MPL-2.0
// Audit-only negative cases. No production policy is changed by these tests.
// Included after the real Vulkan render/copy helpers inside skyline::gpu.
void CheckPhysicalAuthority(GPU &gpu) {
    for (bool freed : {false, true}) for (unsigned scenario = 0; scenario < 5; ++scenario) {
        *gpu.state.settings->freeGuestTextureMemory = freed;
        std::vector<u8> storage(131072 + 4096, 0x2D);
        auto *data = util::AlignUp(storage.data(), 4096);
        constexpr size_t aSize = 65536, bOffset = 49152, bSize = 65536;
        constexpr size_t sharedSize = aSize - bOffset;
        TextureManager manager{gpu};
        auto descriptor = [&](u32 width, u32 height, size_t offset, size_t size) {
            GuestTexture value;
            value.format = format::R8G8B8A8Unorm;
            value.dimensions = vk::Extent3D{width,height,1};
            value.viewType = vk::ImageViewType::e2D;
            value.mipLevelCount = value.viewMipCount = value.layerCount = 1;
            value.layerStride = size;
            value.mappings = {{data + offset,size}};
            return value;
        };
        const auto aDesc = descriptor(128,128,0,aSize);
        const auto bDesc = descriptor(128,128,bOffset,bSize);
        const auto cDesc = descriptor(128,256,0,131072);
        auto a = manager.FindOrCreate(aDesc);
        WriteAliasRenderTarget(a->texture,{1,0,0,1});
        a->texture->cycle->Wait(); // Complete the GPU producer without encoding guest bytes.
        assert(a->texture->memoryFreed == freed);
        if (scenario == 4) {
            // Contained, non-mip overlap falls out of the matcher without
            // entering matches. B uploads the pre-write guest sentinel/zero.
            auto inner = manager.FindOrCreate(descriptor(64,64,4096,16384));
            assert(inner->texture != a->texture && !inner->texture->HasAliases());
            assert(a->texture->gpuReadbackSerial < a->texture->gpuWriteSerial);
            std::vector<u8> old(16384,freed ? 0 : 0x2D), red(16384);
            EncodeAliasBase(inner->texture,red,{255,0,0,255});
            AliasRead(inner->texture);
            VerifyAliasHost(inner->texture,old);
            assert(old != red);
            // Control only: force the missing recovery and invalidate B.
            { std::scoped_lock lock{*a->texture}; a->texture->SynchronizeGuest(false,true); }
            inner->texture->dirtyState = Texture::DirtyState::CpuDirty;
            AliasRead(inner->texture); VerifyAliasHost(inner->texture,red);
            std::printf("OBSERVED free=%u case=contained-non-mip stale_upload_bytes=16384 guest_byte=%u explicit-recovery-control=correct\n",unsigned(freed),freed ? 0U : 45U);
            gpu.scheduler.Complete();
            continue;
        }
        auto b = manager.FindOrCreate(bDesc); // Actual fallback encodes A, once.
        assert(a->texture != b->texture && !a->texture->HasAliases() && !b->texture->HasAliases());
        assert(a->texture->gpuReadbackSerial == a->texture->gpuWriteSerial);
        std::vector<u8> aRed(aSize), bGreen(bSize), aBlue(aSize);
        EncodeAliasBase(a->texture,aRed,{255,0,0,255});
        EncodeAliasBase(b->texture,bGreen,{0,255,0,255});
        EncodeAliasBase(a->texture,aBlue,{0,0,255,255});
        assert(std::equal(aRed.begin(),aRed.end(),data));
        AliasRead(b->texture);
        std::vector<u8> bInitial(bSize,0x2D);
        std::copy(aRed.begin()+bOffset,aRed.end(),bInitial.begin());
        VerifyAliasHost(b->texture,bInitial);
        WriteAliasRenderTarget(b->texture,{0,1,0,1});
        b->texture->cycle->Wait();
        const auto bSerial = b->texture->gpuWriteSerial;
        assert(b->texture->dirtyState == Texture::DirtyState::GpuDirty);
        assert(b->texture->memoryFreed == freed);

        if (scenario == 0) {
            // A -> B -> cached A: no new image, no persistent invalidation.
            auto expected = aRed;
            std::copy(bGreen.begin(),bGreen.begin()+sharedSize,expected.begin()+bOffset);
            auto cached = manager.FindOrCreate(aDesc);
            assert(cached->texture == a->texture && !cached->texture->aliasStale);
            AliasRead(cached->texture); // RequireAliasCoherence + SynchronizeHost.
            VerifyAliasHost(cached->texture,aRed); // Actual old bytes, not desired.
            assert(expected != aRed);
            // Positive control: a new consumer does encode B before upload.
            auto c = manager.FindOrCreate(cDesc);
            std::vector<u8> cExpected(131072,0x2D);
            std::copy(expected.begin(),expected.end(),cExpected.begin());
            std::copy(bGreen.begin(),bGreen.end(),cExpected.begin()+bOffset);
            AliasRead(c->texture);
            VerifyAliasHost(c->texture,cExpected);
            std::printf("OBSERVED free=%u case=cached-A stale_shared_bytes=%zu new-consumer=correct\n",unsigned(freed),sharedSize);
        } else if (scenario == 1 || scenario == 3) {
            // The real interval index visits descending addresses: B then A.
            // Full clears are safe only when that order happens to match writes.
            WriteAliasRenderTarget(a->texture,{0,0,1,1});
            a->texture->cycle->Wait();
            assert(a->texture->gpuWriteSerial > bSerial);
            if (scenario == 3) {
                WriteAliasRenderTarget(b->texture,{0,1,0,1});
                b->texture->cycle->Wait();
            }
            auto c = manager.FindOrCreate(cDesc);
            std::vector<u8> observed(131072,0x2D), wanted(observed);
            std::copy(bGreen.begin(),bGreen.end(),wanted.begin()+bOffset);
            std::copy(aBlue.begin(),aBlue.end(),wanted.begin());
            observed = wanted;
            if (scenario == 3) std::copy(bGreen.begin(),bGreen.end(),wanted.begin()+bOffset);
            assert((observed != wanted) == (scenario == 3));
            assert(std::equal(observed.begin(),observed.end(),data));
            AliasRead(c->texture);
            VerifyAliasHost(c->texture,observed);
            std::printf("%s free=%u case=full-clears shared_span_bytes=%zu A_serial=%llu B_serial=%llu\n",scenario == 3 ? "OBSERVED stale-new-consumer" : "CONTROL correct-new-consumer",unsigned(freed),sharedSize,
                static_cast<unsigned long long>(a->texture->gpuWriteSerial),static_cast<unsigned long long>(b->texture->gpuWriteSerial));
        } else {
            // A writes only its first GOB. B remains newest for the shared span.
            // Whole-image serial sorting cannot express these two authorities.
            WriteAliasRenderTarget(a->texture,{0,0,1,1},vk::Rect2D{{0,0},{16,8}});
            a->texture->cycle->Wait();
            assert(a->texture->gpuWriteSerial > bSerial);
            auto aPartial = aRed;
            std::copy(aBlue.begin(),aBlue.begin()+512,aPartial.begin());
            VerifyAliasHost(a->texture,aPartial);
            std::vector<u8> correct(131072,0x2D), observed(correct);
            std::copy(aPartial.begin(),aPartial.end(),correct.begin());
            std::copy(bGreen.begin(),bGreen.end(),correct.begin()+bOffset);
            observed = correct;
            std::copy(aPartial.begin(),aPartial.end(),observed.begin());
            auto c = manager.FindOrCreate(cDesc);
            assert(observed != correct && std::equal(observed.begin(),observed.end(),data));
            AliasRead(c->texture);
            VerifyAliasHost(c->texture,observed);
            std::printf("OBSERVED free=%u case=partial-A obsolete_guest_span=%zu serial-sorted-whole-readbacks-also-lose-B\n",unsigned(freed),sharedSize);
        }
        gpu.scheduler.Complete();
    }
    *gpu.state.settings->freeGuestTextureMemory = false;
}
