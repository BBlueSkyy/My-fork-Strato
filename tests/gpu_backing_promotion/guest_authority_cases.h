// SPDX-License-Identifier: MPL-2.0
// Included inside skyline::gpu after the existing real-Vulkan alias helpers.
// Audit cases retain the baseline's rejection results; they are not assertions
// that those rejections or the exposed padding loss are repaired.
void CheckGuestAuthorityMatrix(GPU &gpu) {
    for (unsigned scenario = 0; scenario < 7; ++scenario) for (bool explicitLayout : {false,true}) {
        const bool padded = scenario == 6;
        const u32 stride = padded ? 90112 : 65536;
        std::vector<u8> bytes(stride,0xA5), expected(bytes);
        auto make = [&](u32 width) {
            auto t = std::make_shared<Texture>(gpu,vk::Format::eR8G8B8A8Unorm,vk::Extent3D{width,128,1},1,1);
            t->guest->layerStride = stride; t->guest->viewMipCount = 1;
            t->guest->viewType = vk::ImageViewType::e2DArray;
            t->guest->mappings = {span<u8>{bytes}}; t->mirror = bytes;
            t->dirtyState = Texture::DirtyState::CpuDirty;
            std::scoped_lock lock{*t}; t->SynchronizeHost(false);
            return t;
        };
        *gpu.state.settings->freeGuestTextureMemory = false;
        auto writer = make(128), alternate = make(64);
        *gpu.state.settings->freeGuestTextureMemory = true;
        WriteAliasRenderTarget(writer,{0,0,1,1});
        for (size_t i = 0; i < 65536; ++i) expected[i] = std::array<u8,4>{0,0,255,255}[i%4];
        assert(writer->memoryFreed && writer->dirtyState == Texture::DirtyState::GpuDirty);
        auto producer = writer->cycle;
        NativeFormat incompatible{*writer->format};
        if (scenario == 1) { // Already completed guest encoding is eligible.
            std::scoped_lock lock{*writer}; writer->SynchronizeGuest(false,true);
        } else if (scenario == 2) { // No lossless reverse-format source.
            writer->format = &incompatible;
        } else if (scenario == 3) { // Eviction flag without any usable writer.
            writer->dirtyState = Texture::DirtyState::Clean;
            writer->gpuWriteSerial = writer->gpuReadbackSerial = 0;
        } else if (scenario == 4 || scenario == 5) {
            auto old = std::make_shared<texture::AliasGroup>();
            old->generation = 3; old->guestGeneration = scenario == 4 ? 2 : 3;
            old->members = {writer,alternate}; old->writer = alternate; old->writerIdentity = alternate.get();
            writer->aliasGroup.store(old); alternate->aliasGroup.store(old);
            writer->aliasStale = true;
            if (scenario == 5) {
                bytes = expected;
                alternate->gpuWriteSerial = writer->gpuWriteSerial + 1;
                alternate->gpuReadbackSerial = alternate->gpuWriteSerial;
                alternate->dirtyState = Texture::DirtyState::Clean;
            }
        }
        const auto before = bytes;
        bool rejected = false;
        try { Texture::RegisterAliases({writer,alternate},explicitLayout ? &*writer->guest : nullptr); }
        catch (const std::runtime_error&) { rejected = true; }
        const bool expectedReject = scenario == 2 || scenario == 3 || scenario == 4 ||
            ((!explicitLayout) && (scenario == 0 || scenario == 5 || scenario == 6));
        if (rejected != expectedReject) std::fprintf(stderr,"matrix mismatch case=%u explicit=%u rejected=%u expected=%u\n",scenario,unsigned(explicitLayout),unsigned(rejected),unsigned(expectedReject));
        assert(rejected == expectedReject);
        if (rejected) {
            assert(bytes == before);
            assert(!writer->aliasRetired && !writer->replaced);
        } else if (!padded) {
            assert(bytes == expected);
            auto group = alternate->aliasGroup.load();
            assert(group && group->guestGeneration == group->generation);
        } else {
            // The current explicit restore returns success although padding
            // that FreeGuest removed cannot be reconstructed by CopyToGuest.
            assert(std::equal(expected.begin(),expected.begin()+65536,bytes.begin()));
            assert(std::all_of(bytes.begin()+65536,bytes.end(),[](u8 v){return v==0;}));
            std::puts("OBSERVED padded explicit bridge: 24576 sentinel bytes lost; no blanket memoryFreed-guard removal justified");
        }
        std::printf("authority case=%u explicit=%u rejected=%u producer_waits=%u\n",scenario,unsigned(explicitLayout),unsigned(rejected),producer ? producer->gpuWaits : 0);
        writer->format = writer->guest->format;
        writer->aliasGroup.store({}); alternate->aliasGroup.store({});
        *gpu.state.settings->freeGuestTextureMemory = false;
        gpu.scheduler.Complete();
    }
}
