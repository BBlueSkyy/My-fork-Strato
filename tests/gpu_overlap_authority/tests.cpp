// SPDX-License-Identifier: MPL-2.0
// Regression: real Texman, Vulkan writes/readback and page traps must preserve byte authority.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <stdexcept>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

// A real shared allocation, never freed/remapped during a fixture. Guest and
// mirror are distinct virtual mappings of the same physical bytes, as on NCE.
struct SharedGuestMemory {
    static inline SharedGuestMemory *active{};
    const size_t size;
    int fd;
    uint8_t *guest{}, *observer{};
    explicit SharedGuestMemory(size_t pages = 5) : size{pages * 4096}, fd{memfd_create("strato-overlap-authority", 0)} {
        assert(fd >= 0 && !ftruncate(fd, size));
        guest = static_cast<uint8_t *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
        observer = static_cast<uint8_t *>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
        assert(guest != MAP_FAILED && observer != MAP_FAILED);
        std::fill_n(observer,size,0x2D);
        active = this;
    }
    ~SharedGuestMemory() {
        assert(!munmap(guest,size) && !munmap(observer,size));
        assert(!close(fd)); active = nullptr;
    }
};
#include "backing_harness.inc"

namespace skyline::gpu {
    constexpr size_t page{4096}, textureBytes{8192};
    const std::array<u8,4> oldColor{255,0,0,255}, pendingColor{255,0,255,255}, newColor{0,255,0,255};

    GuestTexture Describe(u8 *address) {
        GuestTexture value;
        value.format = format::R8G8B8A8Unorm;
        value.dimensions = vk::Extent3D{64,32,1};
        value.viewType = vk::ImageViewType::e2D;
        value.layerCount = value.mipLevelCount = value.viewMipCount = 1;
        value.layerStride = textureBytes;
        value.tileConfig = {.mode=texture::TileMode::Block,.blockHeight=1,.blockDepth=1};
        value.mappings = {{address,textureBytes}};
        return value;
    }
    void CheckColor(const u8 *bytes, size_t size, const std::array<u8,4> &color) {
        for (size_t i{};i<size;++i) assert(bytes[i] == color[i%4]);
    }
    void CheckGuards(const SharedGuestMemory &arena) {
        for (size_t i{};i<page;++i) {
            assert(arena.observer[i] == 0x2D);
            assert(arena.observer[4*page+i] == 0x2D);
        }
    }
    std::vector<u8> DownloadEncoded(const std::shared_ptr<Texture> &texture) {
        auto staging{texture->gpu.memory.AllocateStagingBuffer(texture->surfaceSize)};
        if (texture->cycle) texture->cycle->WaitSubmit();
        auto cycle{texture->gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            texture->CopyIntoStagingBuffer(commands,staging);
        })};
        cycle->AttachObjects(staging,texture); cycle->Wait(true);
        std::vector<u8> encoded(textureBytes,0x99);
        texture::CopyLinearToBlockLinear(texture->dimensions,1,1,4,texture->guest->tileConfig.blockHeight,texture->guest->tileConfig.blockDepth,staging->data(),encoded.data(),texture->guest->tileConfig.tileWidthSpacing);
        return encoded;
    }
    void CheckComposite(const SharedGuestMemory &arena, bool correctIntersection,
                        const std::array<u8,4> &aColor = oldColor) {
        CheckColor(arena.observer+page,page,aColor); // A-only
        CheckColor(arena.observer+2*page,page,correctIntersection ? newColor : aColor);
        CheckColor(arena.observer+3*page,page,newColor); // B-only
        CheckGuards(arena);
    }

    // Break caught: old A readback/reuse must not replace the newer B intersection.
    void CheckPartialAuthority(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,
                               u32 family,bool stalePending,bool preReadB,bool trapWrite,bool reuseFirst,bool freeGuest) {
        SharedGuestMemory arena;
        GPU gpu{device,physical,family,true};
        TextureManager manager{gpu};
        *gpu.state.settings->freeGuestTextureMemory = freeGuest;
        gpu.state.process->memory.allowEviction = freeGuest;
        const auto descriptorA{Describe(arena.guest+page)};
        const auto descriptorB{Describe(arena.guest+2*page)};
        auto a{manager.FindOrCreate(descriptorA)};
        WriteAliasRenderTarget(a->texture,{1,0,0,1});
        auto b{manager.FindOrCreate(descriptorB)};
        AliasRead(b->texture); // B is prepared while the guest intersection is red.
        const auto registeredDiscards{gpu.state.process->memory.discards};
        assert(a->texture != b->texture && !a->texture->HasAliases() && !b->texture->HasAliases());
        const auto imageA{a->texture->GetBacking()}, imageB{b->texture->GetBacking()};
        assert(imageA != imageB);
        const auto aColor{stalePending ? pendingColor : oldColor};
        if (stalePending) WriteAliasRenderTarget(a->texture,{1,0,1,1});
        // A real full GPU overwrite of an already prepared B needs no old B
        // texels. Skipping the test helper's extra read preparation keeps A's
        // new exclusive channel GPU-only, so masked readback is exercised.
        WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
        b->texture->cycle->Wait();
        if (preReadB) {
            std::scoped_lock lock{*b->texture};
            b->texture->SynchronizeGuest(false,false);
            CheckColor(arena.observer+2*page,textureBytes,newColor);
        }
        auto reuseA{[&] {
            auto reused{manager.FindOrCreate(descriptorA)};
            assert(reused->texture == a->texture);
            AliasRead(reused->texture);
            const auto actual{DownloadEncoded(reused->texture)};
            CheckColor(actual.data(),page,aColor);
            CheckColor(actual.data()+page,page,newColor);
        }};
        if (reuseFirst) reuseA();
        auto &trap{gpu.state.process->trap};
        trap.events.clear();
        assert(trap.manager.HandleTrap(arena.guest+2*page,trapWrite));
        // NCE forwards reads as write traps too. No CPU store follows either mode.
        volatile const u8 consumed{arena.guest[2*page]};
        assert(consumed == newColor[0]);
        CheckComposite(arena,true,aColor);
        std::printf("Callbacks (%s A=%s B=%s %s):",reuseFirst ? "reuse-first" : "trap-first",
            stalePending ? "pending" : "clean",preReadB ? "clean" : "pending",trapWrite ? "write" : "read");
        for (auto [id,kind] : trap.events) std::printf(" %c:%c",id == *a->texture->trapHandle ? 'A' : 'B',kind);
        std::puts("");
        reuseA();
        auto reusedB{manager.FindOrCreate(descriptorB)};
        AliasRead(reusedB->texture);
        auto actualB{DownloadEncoded(reusedB->texture)};
        CheckColor(actualB.data(),textureBytes,newColor);
        auto submissions{gpu.scheduler.submissions};
        for (unsigned repeat{};repeat<3;++repeat) { AliasRead(a->texture); AliasRead(b->texture); }
        assert(gpu.scheduler.submissions == submissions); // No extra copies after reconciliation.

        // A→B→A: a partial A write must load/preserve the current green intersection,
        // including when B was already Clean. Top 16 rows are A's first 4096 bytes.
        WriteAliasRenderTarget(a->texture,{1,1,0,1},true,vk::Rect2D{{0,0},{64,16}});
        { std::scoped_lock lock{*b->texture}; b->texture->SynchronizeGuest(false,false); }
        AliasRead(b->texture);
        actualB=DownloadEncoded(b->texture);
        CheckColor(actualB.data(),textureBytes,newColor);
        AliasRead(a->texture);
        const auto finalA{DownloadEncoded(a->texture)};
        const std::array<u8,4> yellow{255,255,0,255};
        CheckColor(finalA.data(),page,yellow);
        CheckColor(finalA.data()+page,page,newColor);
        { std::scoped_lock lock{*a->texture}; a->texture->SynchronizeGuest(false,false); }
        CheckColor(arena.observer+page,page,yellow);
        CheckColor(arena.observer+2*page,textureBytes,newColor);
        CheckGuards(arena);
        assert(a->texture->GetBacking()==imageA && b->texture->GetBacking()==imageB);
        assert(!a->texture->replaced && !b->texture->replaced);
        assert(gpu.state.process->memory.discards==registeredDiscards);
        assert(*gpu.state.settings->freeGuestTextureMemory == freeGuest && !*gpu.state.settings->enableFastGpuReadbackHack);
        gpu.scheduler.Complete();
        assert(!validationErrors);
    }

    void CheckPaddingAndFailure(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena;
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto a{manager.FindOrCreate(Describe(arena.guest+page))};
        WriteAliasRenderTarget(a->texture,{1,0,0,1});
        auto tiny{Describe(arena.guest+2*page+16)};
        tiny.dimensions=vk::Extent3D{8,1,1}; tiny.layerStride=512; tiny.mappings={{arena.guest+2*page+16,512}};
        auto b{manager.FindOrCreate(tiny)}; AliasRead(b->texture);
        *gpu.state.settings->freeGuestTextureMemory=true; // Known shared guest bytes must stay intact.
        WriteAliasRenderTarget(a->texture,{1,0,1,1});
        WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
        const std::vector<u8> before(arena.observer,arena.observer+arena.size);
        gpu.scheduler.failRecording=true;
        bool failed{}; try { b->texture->SynchronizeGuest(false,false); } catch (const std::bad_alloc &) { failed=true; }
        gpu.scheduler.failRecording=false;
        assert(failed && std::equal(before.begin(),before.end(),arena.observer));
        assert(b->texture->dirtyState==Texture::GpuDirty && b->texture->gpuReadbackSerial<b->texture->gpuWriteSerial);
        b->texture->SynchronizeGuest(false,false);
        a->texture->SynchronizeGuest(false,false);
        // Independent literal Maxwell addresses for the two 16-byte sectors of
        // one 8x1 RGBA row. All the rest, including B's padding, belongs to A.
        for (size_t offset{};offset<textureBytes;++offset) {
            bool green{(offset>=page+16 && offset<page+32) || (offset>=page+48 && offset<page+64)};
            assert(arena.observer[page+offset] == (green ? newColor[offset%4] : pendingColor[offset%4]));
        }
        auto &trap{gpu.state.process->trap};
        std::promise<void> locked,release; auto released{release.get_future()};
        auto group{a->texture->overlapState->group.load()};
        WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
        b->texture->cycle->Wait(); b->texture->cycle.reset();
        std::thread holder{[&] { std::scoped_lock operation{group->operationMutex}; locked.set_value(); released.wait(); }};
        locked.get_future().wait();
        const bool deferred{!trap.callbacks[*b->texture->trapHandle].read()};
        release.set_value(); holder.join(); assert(deferred);
        b->texture->SynchronizeGuest(false,false);
        a->texture->ReconcileAliases();
        gpu.memory.failAllocation=true; failed=false;
        try { AliasRead(a->texture); } catch (const std::bad_alloc &) { failed=true; }
        gpu.memory.failAllocation=false;
        assert(failed && a->texture->dirtyState==Texture::CpuDirty);
        AliasRead(a->texture);
        const auto actual{DownloadEncoded(a->texture)};
        for (size_t offset{};offset<textureBytes;++offset) {
            bool green{(offset>=page+16 && offset<page+32) || (offset>=page+48 && offset<page+64)};
            assert(actual[offset] == (green ? newColor[offset%4] : pendingColor[offset%4]));
        }
        // A real CPU store after a write trap must become guest authority too.
        assert(trap.manager.HandleTrap(arena.guest+2*page+16,true));
        arena.guest[2*page+16]=0x7B;
        auto commands{gpu.scheduler.AllocateCommandBuffer()};
        commands->begin(vk::CommandBufferBeginInfo{});
        gpu.memory.failAllocation=true; failed=false;
        try { a->texture->SynchronizeHostInline(*commands,commands.cycle,true); } catch (const std::bad_alloc &) { failed=true; }
        gpu.memory.failAllocation=false;
        assert(failed && a->texture->dirtyState==Texture::CpuDirty);
        commands->end(); commands.cycle->Cancel();
        AliasRead(a->texture); AliasRead(b->texture);
        const auto stored{DownloadEncoded(a->texture)};
        assert(stored[page+16]==0x7B);
        // Non-trap guest invalidation must retain a reusable trap handle.
        a->texture->SynchronizeGuest(true,false);
        WriteAliasRenderTarget(a->texture,{1,0,1,1});
        AliasRead(b->texture);
        const auto small{DownloadEncoded(b->texture)};
        CheckColor(small.data(),16,pendingColor); CheckColor(small.data()+32,16,pendingColor);
        CheckGuards(arena); assert(gpu.state.process->memory.discards==0);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: texel/padding ownership, failed readback/upload retry, nonblocking partial trap, shared-byte eviction suppression");
    }

    void CheckExecutorRetry(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena;
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto da{Describe(arena.guest+page)}, db{Describe(arena.guest+2*page)};
        auto a{manager.FindOrCreate(da)}; AliasRead(a->texture);
        gpu.scheduler.deferSubmission=true;
        WriteAliasRenderTarget(a->texture,{1,0,0,1});
        assert(!a->texture->cycle->submitted);
        interconnect::CommandExecutor executor;
        executor.onSubmit=[&] { gpu.scheduler.FlushPending(); };
        std::shared_ptr<TextureView> b;
        texture::RetryBackingPromotion([&] { b=manager.FindOrCreate(db,executor.tag); },
            [&](const auto &request) { executor.HandleTextureBackingPromotion(request); });
        assert(executor.submissions==1 && a->texture->HasOverlaps() && b->texture->HasOverlaps());
        WriteAliasRenderTarget(b->texture,{0,1,0,1});
        const auto submissions{executor.submissions};
        texture::RetryBackingPromotion([&] { executor.AttachTexture(a.get()); },
            [&](const auto &request) { executor.HandleTextureBackingPromotion(request); });
        assert(executor.submissions==submissions+1);
        const auto actual{DownloadEncoded(a->texture)};
        CheckColor(actual.data(),page,oldColor); CheckColor(actual.data()+page,page,newColor);
        for (unsigned i{};i<3;++i) executor.AttachTexture(a.get());
        assert(executor.submissions==submissions+1);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: production manager/AttachTexture submit-retry registration and cached reuse");
    }

    void CheckExactPeers(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena;
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto descriptor{Describe(arena.guest+page)};
        auto a{manager.FindOrCreate(descriptor)}; WriteAliasRenderTarget(a->texture,{1,0,0,1});
        auto alternate{descriptor}; alternate.tileConfig.blockHeight=2;
        auto exact{manager.FindOrCreate(alternate)}; AliasRead(exact->texture);
        assert(a->texture!=exact->texture && a->texture->aliasGroup.load()==exact->texture->aliasGroup.load());
        auto b{manager.FindOrCreate(Describe(arena.guest+2*page))}; AliasRead(b->texture);
        WriteAliasRenderTarget(exact->texture,{1,0,1,1});
        WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
        AliasRead(a->texture);
        const auto composed{DownloadEncoded(a->texture)};
        CheckColor(composed.data(),page,pendingColor); CheckColor(composed.data()+page,page,newColor);
        WriteAliasRenderTarget(a->texture,{0,1,1,1});
        AliasRead(exact->texture); AliasRead(b->texture);
        const std::array<u8,4> cyan{0,255,255,255};
        const auto exactBytes{DownloadEncoded(exact->texture)}, bBytes{DownloadEncoded(b->texture)};
        CheckColor(exactBytes.data(),textureBytes,cyan);
        CheckColor(bBytes.data(),page,cyan); CheckColor(bBytes.data()+page,page,newColor);
        CheckGuards(arena); gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: exact alias peers inside a partial-overlap component");
    }

    void CheckFragmentedMappings(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena{7};
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto da{Describe(arena.guest+page)}, db{Describe(arena.guest+3*page)};
        da.mappings={{arena.guest+page,page},{arena.guest+3*page,page}};
        db.mappings={{arena.guest+3*page,page},{arena.guest+5*page,page}};
        auto a{manager.FindOrCreate(da)}; WriteAliasRenderTarget(a->texture,{1,0,0,1});
        auto b{manager.FindOrCreate(db)}; AliasRead(b->texture);
        WriteAliasRenderTarget(a->texture,{1,0,1,1}); WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
        assert(gpu.state.process->trap.manager.HandleTrap(arena.guest+3*page,true));
        CheckColor(arena.observer+page,page,pendingColor);
        CheckColor(arena.observer+3*page,page,newColor);
        CheckColor(arena.observer+5*page,page,newColor);
        AliasRead(a->texture); AliasRead(b->texture);
        const auto bytes{DownloadEncoded(a->texture)};
        CheckColor(bytes.data(),page,pendingColor); CheckColor(bytes.data()+page,page,newColor);
        for (size_t empty : {0U,2U,4U,6U})
            for (size_t i{};i<page;++i) assert(arena.observer[empty*page+i]==0x2D);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: fragmented mappings and later-segment intersection");
    }

    void PrepareCachedBindings(interconnect::CommandExecutor &executor,span<TextureView *> views) {
        struct { interconnect::CommandExecutor &executor; } ctx{executor};
        // Exact early-preparation guard from production PrepareDraw. Android
        // pipeline/state-builder boundaries are not reproduced by this facade.
        #include "prepare_coherence.inc"
        requireCoherence(views);
    }

    void CheckCachedDescriptorReuse(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena;
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto a{manager.FindOrCreate(Describe(arena.guest+page))}; WriteAliasRenderTarget(a->texture,{1,0,0,1});
        auto b{manager.FindOrCreate(Describe(arena.guest+2*page))}; AliasRead(b->texture);
        AliasRead(a->texture);
        // A's already bound descriptor survives B's subsequent RT write. Quick
        // bind neither fetches the TIC nor calls AttachTexture again.
        WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
        assert(a->texture->overlapStale);
        CheckColor(DownloadEncoded(a->texture).data(),textureBytes,oldColor);
        interconnect::CommandExecutor executor;
        std::array<TextureView *,1> sampled{a.get()};
        bool retry{};
        try { PrepareCachedBindings(executor,sampled); executor.CreateRenderPassWithSubpass({{0,0},{64,32}},sampled,{}, {},nullptr,false,{},{}); }
        catch (const TextureAliasReconciliationRequest &request) {
            retry=true;
            assert(executor.localSlot.nodes.empty()); // Reject before publishing this draw.
            executor.HandleTextureBackingPromotion(request);
        }
        assert(retry); // Cached descriptors must not consume the old VkImage.
        AliasRead(a->texture);
        const auto bytes{DownloadEncoded(a->texture)};
        CheckColor(bytes.data(),page,oldColor); CheckColor(bytes.data()+page,page,newColor);
        executor.CreateRenderPassWithSubpass({{0,0},{64,32}},sampled,{}, {},nullptr,false,{},{});
        const auto submissions{gpu.scheduler.submissions};
        PrepareCachedBindings(executor,sampled);
        executor.CreateRenderPassWithSubpass({{0,0},{64,32}},sampled,{}, {},nullptr,false,{},{});
        assert(gpu.scheduler.submissions==submissions); // Coherent reuse never submits.
        WriteAliasRenderTarget(a->texture,{1,0,1,1});
        assert(b->texture->overlapStale);
        std::array<TextureView *,1> outputs{b.get()};
        const auto nodes{executor.localSlot.nodes.size()};
        retry=false;
        try { PrepareCachedBindings(executor,outputs); executor.CreateRenderPassWithSubpass({{0,0},{64,32}},{},{},outputs,nullptr,false,{},{}); }
        catch (const TextureAliasReconciliationRequest &request) {
            retry=true; assert(executor.localSlot.nodes.size()==nodes);
            executor.HandleTextureBackingPromotion(request);
        }
        assert(retry); AliasRead(b->texture);
        const auto target{DownloadEncoded(b->texture)};
        CheckColor(target.data(),page,pendingColor); CheckColor(target.data()+page,page,newColor);
        CheckGuards(arena); gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: production early preparation rejects stale cached descriptor/RT before nodes; coherent reuse never submits");
    }

    void CheckEvictedSampledOverlap(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        for (bool context : {false,true}) {
        SharedGuestMemory arena;
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        for (size_t i{};i<textureBytes;++i) arena.observer[page+i]=oldColor[i%4];
        auto a{manager.FindOrCreate(Describe(arena.guest+page))};
        *gpu.state.settings->freeGuestTextureMemory=true;
        gpu.state.process->memory.allowEviction=true;
        a->texture->SynchronizeHost(true); // Sampled upload, no actual GPU writer.
        assert(a->texture->memoryFreed && !a->texture->gpuWriteSerial);
        assert(gpu.state.process->memory.discards==1);
        interconnect::CommandExecutor executor;
        std::shared_ptr<TextureView> b;
        bool failReadback{};
        auto lookup{[&] {
            texture::RetryBackingPromotion([&] { b=manager.FindOrCreate(Describe(arena.guest+2*page),context ? 1 : 0); },
                [&](const auto &request) {
                    gpu.scheduler.failRecording=failReadback;
                    executor.HandleTextureBackingPromotion(request);
                });
        }};
        if (context) {
            failReadback=true;
            bool failed{}; try { lookup(); } catch (const std::bad_alloc &) { failed=true; }
            failReadback=false; gpu.scheduler.failRecording=false;
            assert(failed && !b && a->texture->memoryFreed && a->texture->HasPendingOverlapWrite());
        }
        lookup(); AliasRead(b->texture);
        const auto bytes{DownloadEncoded(b->texture)};
        CheckColor(bytes.data(),page,oldColor);
        for (size_t i{page};i<textureBytes;++i) assert(bytes[i]==0x2D);
        CheckColor(arena.observer+page,textureBytes,oldColor);
        assert(!a->texture->memoryFreed && gpu.state.process->memory.discards==1);
        const auto submissions{gpu.scheduler.submissions};
        AliasRead(b->texture); AliasRead(b->texture);
        assert(gpu.scheduler.submissions==submissions && !a->texture->gpuWriteSerial);
        WriteAliasRenderTarget(b->texture,{0,1,0,1});
        AliasRead(a->texture);
        const auto composite{DownloadEncoded(a->texture)};
        CheckColor(composite.data(),page,oldColor);
        CheckColor(composite.data()+page,page,newColor);
        CheckGuards(arena); gpu.scheduler.Complete(); assert(!validationErrors);
        }
        std::puts("PASS: first partial overlap restores previously evicted sampled-only backing");
    }

    void CheckLateExactAlias(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        for (bool clean : {false,true}) for (bool context : {false,true}) {
            SharedGuestMemory arena;
            GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
            auto small{Describe(arena.guest+2*page)}; small.dimensions=vk::Extent3D{32,32,1};
            small.layerStride=4096; small.mappings={{arena.guest+2*page,4096}};
            auto a{manager.FindOrCreate(small)}; WriteAliasRenderTarget(a->texture,{1,0,0,1});
            auto b{manager.FindOrCreate(Describe(arena.guest+page))}; AliasRead(b->texture);
            WriteAliasRenderTarget(a->texture,{1,0,1,1}); WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
            if (clean) b->texture->SynchronizeGuest(false,true);
            const auto oldReadback{a->texture->gpuReadbackSerial};
            auto alternate{small}; alternate.tileConfig.blockHeight=2;
            interconnect::CommandExecutor executor;
            std::shared_ptr<TextureView> c;
            auto lookup{[&] {
                texture::RetryBackingPromotion([&] { c=manager.FindOrCreate(alternate,context ? 1 : 0); },
                    [&](const auto &request) { executor.HandleTextureBackingPromotion(request); });
            }};
            if (!clean) {
                const std::vector<u8> before(arena.observer,arena.observer+5*page);
                gpu.scheduler.failRecording=true;
                bool failed{}; try { lookup(); } catch (const std::bad_alloc &) { failed=true; }
                gpu.scheduler.failRecording=false;
                assert(failed && !c && std::equal(before.begin(),before.end(),arena.observer));
                assert(a->texture->gpuReadbackSerial==oldReadback);
            }
            lookup(); assert(c && c->texture!=a->texture);
            assert(a->texture->gpuReadbackSerial==oldReadback); // Never fabricate A's readback.
            AliasRead(c->texture); CheckColor(DownloadEncoded(c->texture).data(),4096,newColor);
            CheckColor(arena.observer+page,textureBytes,newColor);
            WriteAliasRenderTarget(c->texture,{1,1,0,1});
            AliasRead(b->texture);
            const auto bytes{DownloadEncoded(b->texture)};
            CheckColor(bytes.data(),page,newColor);
            CheckColor(bytes.data()+page,page,std::array<u8,4>{255,255,0,255});
            CheckGuards(arena);
            gpu.scheduler.Complete(); assert(!validationErrors);
        }
        std::puts("PASS: late exact aliases use current interval authority, preserve serials and retry failed publication");
    }

    void ReadSelected(const std::shared_ptr<TextureView> &view) {
        texture::RetryBackingPromotion([&] {
            std::scoped_lock lock{*view->texture};
            view->texture->RequireAliasCoherence(1, &view->range);
            view->texture->SynchronizeHost(false);
        }, [&](const auto &request) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(request)>,TextureAliasReconciliationRequest>)
                request.target->ReconcileAliases();
            else assert(false);
        });
    }

    void WriteSelected(const std::shared_ptr<TextureView> &view, std::array<float,4> color, bool storage = false) {
        ReadSelected(view);
        auto owner{view->texture}; std::scoped_lock lock{*owner};
        if (storage) { UsageTracker usage; owner->MarkGpuDirty(usage, &view->range); }
        else owner->UpdateRenderPassUsage(1,texture::RenderPassUsage::RenderTarget,&view->range);
        auto previous{owner->cycle};
        if (previous) previous->WaitSubmit();
        owner->cycle=owner->gpu.scheduler.Submit([&](vk::raii::CommandBuffer &commands) {
            commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{}, {}, {},
                vk::ImageMemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask=vk::AccessFlagBits::eTransferWrite,
                    .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                    .image=owner->GetBacking(),.subresourceRange=view->range});
            commands.clearColorImage(owner->GetBacking(),vk::ImageLayout::eGeneral,vk::ClearColorValue{color},view->range);
        });
        owner->cycle->AttachObjects(view,owner); owner->cycle->ChainCycle(previous);
    }

    void CheckViewOwnership(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family,
                            bool mips,bool fragmented,bool freeGuest) {
        SharedGuestMemory arena{16};
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        *gpu.state.settings->freeGuestTextureMemory=freeGuest;
        gpu.state.process->memory.allowEviction=true;
        const size_t firstSize{mips ? 16384U : textureBytes}, lastSize{mips ? 4096U : textureBytes};
        const size_t total{firstSize+lastSize};
        auto da{Describe(arena.guest+page)};
        da.dimensions=mips ? vk::Extent3D{64,64,1} : vk::Extent3D{64,32,1};
        da.mipLevelCount=da.viewMipCount=mips ? 2 : 1;
        da.layerCount=mips ? 1 : 2; da.viewType=vk::ImageViewType::e2DArray;
        da.layerStride=mips ? total : firstSize;
        auto *firstAddress{fragmented ? arena.guest+8*page : arena.guest+page};
        auto *shared{fragmented ? arena.guest+page : firstAddress+firstSize};
        // Fragmented mappings deliberately reverse physical and logical order.
        da.mappings=fragmented ? GuestTexture::Mappings{{firstAddress,firstSize},{shared,lastSize}} :
            GuestTexture::Mappings{{firstAddress,total}};
        auto db{da}; db.mappings={{shared,total}};
        auto a{manager.FindOrCreate(da)},b{manager.FindOrCreate(db)};
        AliasRead(a->texture); AliasRead(b->texture);
        const vk::ImageSubresourceRange first{vk::ImageAspectFlagBits::eColor,0,1,0,1};
        const vk::ImageSubresourceRange second{vk::ImageAspectFlagBits::eColor,mips ? 1U : 0U,1,mips ? 0U : 1U,1};
        auto makeView{[&](const auto &texture,const auto &range) {
            return std::make_shared<TextureView>(texture,vk::ImageViewType::e2DArray,range,texture->format,vk::ComponentMapping{});
        }};
        auto a0{makeView(a->texture,first)},a1{makeView(a->texture,second)},b0{makeView(b->texture,first)},b1{makeView(b->texture,second)};
        WriteSelected(a0,{1,0,0,1});
        auto group{a->texture->overlapState->group.load()};
        for (const auto &untouched : a->texture->GetOverlapRanges(&second))
            group->ForEach(untouched,[&](auto,auto,const auto &region) { assert(region.identity!=a->texture.get()); });
        WriteSelected(b0,{0,1,0,1},true);
        assert(a->texture->overlapStale);
        // A0 is current despite its other mip/layer being stale. B's unwritten
        // second subresource must not be claimed by the storage-image write.
        for (const auto &untouched : b->texture->GetOverlapRanges(&second))
            group->ForEach(untouched,[&](auto,auto,const auto &region) { assert(region.identity!=b->texture.get()); });
        interconnect::CommandExecutor executor;
        std::array<TextureView *,2> cached{a0.get(),b0.get()};
        PrepareCachedBindings(executor,cached);
        Texture::ValidateAliasWrites(cached);
        const auto submits{gpu.scheduler.submissions},allocations{gpu.memory.count},staging{gpu.memory.stagingAllocations},discards{gpu.state.process->memory.discards};
        for (unsigned repeat{};repeat<16;++repeat) {
            WriteSelected(a0,{1,0,0,1},repeat%2); WriteSelected(b0,{0,1,0,1},repeat%2);
            PrepareCachedBindings(executor,cached);
        }
        assert(gpu.scheduler.submissions==submits+32 && gpu.memory.count==allocations && gpu.memory.stagingAllocations==staging);
        assert(gpu.state.process->memory.discards==discards);
        // Guest bytes remain old: independent reuse did not read back GPU-only data.
        for (size_t i{};i<firstSize;++i) assert(arena.observer[(firstAddress-arena.guest)+i]==0x2D);
        bool staleViewRejected{};
        try { a->texture->RequireAliasCoherence(1,&second); }
        catch(const TextureAliasReconciliationRequest &) { staleViewRejected=true; }
        assert(staleViewRejected);
        ReadSelected(a1); // Necessary whole-image bridge must preserve A0 as well.
        std::vector<u8> expectedA(total,0x2D),expectedB(total,0x2D);
        for (size_t i{};i<firstSize;++i) { expectedA[i]=oldColor[i%4]; expectedB[i]=newColor[i%4]; }
        for (size_t i{firstSize};i<total;++i) expectedA[i]=newColor[i%4];
        VerifyAliasHost(a->texture,expectedA); VerifyAliasHost(b->texture,expectedB);
        auto snapshot{a->texture->overlapState->synchronizedSerial};
        WriteSelected(b0,{0,0,1,1});
        gpu.memory.failAllocation=true;
        bool failed{};try { ReadSelected(a1); } catch(const std::bad_alloc &) { failed=true; }
        gpu.memory.failAllocation=false;
        assert(failed && a->texture->overlapState->synchronizedSerial==snapshot);
        ReadSelected(a1);
        assert(a->texture->overlapState->synchronizedSerial>snapshot);
        for (size_t i{firstSize};i<total;++i) expectedA[i]=std::array<u8,4>{0,0,255,255}[i%4];
        VerifyAliasHost(a->texture,expectedA);
        // Cache invalidation is independent of the selected footprint object's lifetime.
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::printf("PASS selected %s fragmented=%u free=%u: 32 writes, 32 submissions, zero bridge readbacks/uploads or image/staging allocations; exclusive bytes and failed-upload epoch preserved\n",mips ? "mips" : "layers",unsigned(fragmented),unsigned(freeGuest));
    }

    void CheckCpuUploadWithPendingForeign(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena{9}; GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto da{Describe(arena.guest+page)},db{Describe(arena.guest+3*page)};
        for (auto *d : {&da,&db}) { d->layerCount=2; d->viewType=vk::ImageViewType::e2DArray; d->mappings={{d->mappings.front().data(),2*textureBytes}}; }
        auto a{manager.FindOrCreate(da)},b{manager.FindOrCreate(db)}; AliasRead(a->texture); AliasRead(b->texture);
        const vk::ImageSubresourceRange first{vk::ImageAspectFlagBits::eColor,0,1,0,1},second{vk::ImageAspectFlagBits::eColor,0,1,1,1};
        auto a0{std::make_shared<TextureView>(a->texture,vk::ImageViewType::e2DArray,first,a->texture->format,vk::ComponentMapping{})};
        auto b0{std::make_shared<TextureView>(b->texture,vk::ImageViewType::e2DArray,first,b->texture->format,vk::ComponentMapping{})};
        auto a1{std::make_shared<TextureView>(a->texture,vk::ImageViewType::e2DArray,second,a->texture->format,vk::ComponentMapping{})};
        WriteSelected(a0,{1,0,0,1}); WriteSelected(b0,{0,1,0,1});
        assert(gpu.state.process->trap.manager.HandleTrap(arena.guest+page,true));
        arena.guest[page]=0x91; // Actual CPU store; traps may publish the full overlap component.
        WriteSelected(b0,{0,1,0,1}); // A remains CPU-dirty while B gains a new GPU-only serial.
        assert(a->texture->dirtyState==Texture::DirtyState::CpuDirty && b->texture->HasPendingOverlapWrite());
        ReadSelected(a0); // Whole upload can copy stale guest bytes of the unconsumed A1.
        b->texture->SynchronizeGuest(false,false); // Publication occurs AFTER that upload.
        bool stale{};try { a->texture->RequireAliasCoherence(1,&second); } catch(const TextureAliasReconciliationRequest &) { stale=true; }
        assert(stale); // A later guest publication cannot retroactively repair its VkImage.
        ReadSelected(a1);
        std::vector<u8> expected(2*textureBytes);
        for(size_t i{};i<expected.size();++i) expected[i]=(i<textureBytes ? oldColor : newColor)[i%4];
        expected[0]=0x91; VerifyAliasHost(a->texture,expected);
        gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: real CPU store/full upload with pending foreign GPU bytes cannot fabricate a coherent snapshot");
    }

    void CheckIndependentWriteValidation(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena{9};
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto da{Describe(arena.guest+page)}, db{Describe(arena.guest+3*page)};
        for (auto *d : {&da,&db}) {
            d->layerCount=2; d->viewType=vk::ImageViewType::e2DArray;
            d->mappings={{d->mappings.front().data(),2*textureBytes}};
        }
        auto a{manager.FindOrCreate(da)}, b{manager.FindOrCreate(db)};
        const vk::ImageSubresourceRange first{vk::ImageAspectFlagBits::eColor,0,1,0,1};
        const vk::ImageSubresourceRange second{vk::ImageAspectFlagBits::eColor,0,1,1,1};
        auto a0{std::make_shared<TextureView>(a->texture,vk::ImageViewType::e2DArray,first,a->texture->format,vk::ComponentMapping{})};
        auto a1{std::make_shared<TextureView>(a->texture,vk::ImageViewType::e2DArray,second,a->texture->format,vk::ComponentMapping{})};
        auto b0{std::make_shared<TextureView>(b->texture,vk::ImageViewType::e2DArray,first,b->texture->format,vk::ComponentMapping{})};
        std::array<TextureView *,2> independent{a0.get(),b0.get()};
        bool rejected{};
        try { Texture::ValidateAliasWrites(independent); } catch (const std::runtime_error &) { rejected=true; }
        assert(!rejected); // Backings overlap, but these two written layers do not.
        interconnect::CommandExecutor executor;
        executor.CreateRenderPassWithSubpass({{0,0},{64,32}},{},{},independent,nullptr,false,{},{});
        const auto nodes{executor.localSlot.nodes.size()}, serialA{a->texture->gpuWriteSerial}, serialB{b->texture->gpuWriteSerial};
        std::array<TextureView *,2> conflict{a1.get(),b0.get()};
        rejected=false;
        try { Texture::ValidateAliasWrites(conflict); } catch (const std::runtime_error &) { rejected=true; }
        assert(rejected); // The same physical bytes still cannot have two writers.
        rejected=false;
        try { executor.CreateRenderPassWithSubpass({{0,0},{64,32}},{},{},conflict,nullptr,false,{},{}); }
        catch (const std::runtime_error &) { rejected=true; }
        assert(rejected && executor.localSlot.nodes.size()==nodes && a->texture->gpuWriteSerial==serialA && b->texture->gpuWriteSerial==serialB);
        auto exact{std::make_shared<Texture>(gpu,da)};
        exact->SetupGuestMappings(); exact->TransitionLayout(vk::ImageLayout::eGeneral);
        Texture::RegisterAliases({a->texture,exact},&da);
        Texture::RegisterOverlaps({a->texture,b->texture,exact});
        auto exact0{std::make_shared<TextureView>(exact,vk::ImageViewType::e2DArray,first,exact->format,vk::ComponentMapping{})};
        std::array<TextureView *,2> exactIndependent{exact0.get(),a1.get()};
        Texture::ValidateAliasWrites(exactIndependent); // Regional aliases also use byte-level validation.
        std::array<TextureView *,2> exactConflict{exact0.get(),a0.get()};
        rejected=false;
        try { Texture::ValidateAliasWrites(exactConflict); } catch(const std::runtime_error &) { rejected=true; }
        assert(rejected);
        gpu.scheduler.Complete();
        std::puts("PASS: independent view writes accepted; true physical write conflict rejected, including regional exact peers");
    }

    void CheckFullySupersededAndLayers(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        {
            SharedGuestMemory arena;
            GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
            auto small{Describe(arena.guest+2*page)}; small.dimensions=vk::Extent3D{32,32,1};
            small.layerStride=4096; small.mappings={{arena.guest+2*page,4096}};
            auto a{manager.FindOrCreate(small)}; WriteAliasRenderTarget(a->texture,{1,0,0,1});
            auto b{manager.FindOrCreate(Describe(arena.guest+page))}; AliasRead(b->texture);
            WriteAliasRenderTarget(a->texture,{1,0,1,1}); WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
            const auto submissions{gpu.scheduler.submissions};
            a->texture->SynchronizeGuest(false,false);
            assert(gpu.scheduler.submissions==submissions); // No texels owned: no old-image readback.
            assert(a->texture->gpuReadbackSerial<a->texture->gpuWriteSerial); // No fabricated copy serial.
            assert(gpu.state.process->trap.manager.HandleTrap(arena.guest+2*page,false));
            CheckColor(arena.observer+page,textureBytes,newColor);
            AliasRead(a->texture);
            const auto bytes{DownloadEncoded(a->texture)};
            CheckColor(bytes.data(),4096,newColor); CheckGuards(arena);
            gpu.scheduler.Complete(); assert(!validationErrors);
        }
        for (bool explicitBacking : {false, true}) {
            SharedGuestMemory arena{9};
            GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
            auto da{Describe(arena.guest+page)}, db{Describe(arena.guest+3*page)};
            for (auto *d : {&da,&db}) {
                // Current master separates the viewed layers from the full
                // backing. Both encodings must preserve the same two layers.
                d->layerCount=explicitBacking ? 1 : 2;
                d->backingLayerCount=explicitBacking ? 2 : 0;
                d->viewLayerCount=explicitBacking ? 1 : 0;
                d->viewType=vk::ImageViewType::e2DArray;
                d->mappings={{d->mappings.front().data(),2*textureBytes}};
            }
            auto a{manager.FindOrCreate(da)}; WriteAliasRenderTarget(a->texture,{1,0,0,1});
            auto b{manager.FindOrCreate(db)}; AliasRead(b->texture);
            WriteAliasRenderTarget(a->texture,{1,0,1,1}); WriteAliasRenderTarget(b->texture,{0,1,0,1},false);
            b->texture->SynchronizeGuest(false,false); a->texture->SynchronizeGuest(false,false);
            CheckColor(arena.observer+page,textureBytes,pendingColor);
            CheckColor(arena.observer+3*page,2*textureBytes,newColor);
            AliasRead(a->texture);
            std::vector<u8> expected(2*textureBytes);
            for (size_t i{};i<expected.size();++i) expected[i]=(i<textureBytes ? pendingColor : newColor)[i%4];
            VerifyAliasHost(a->texture,expected);
            for (size_t i{};i<page;++i) assert(arena.observer[i]==0x2D && arena.observer[8*page+i]==0x2D);
            gpu.scheduler.Complete(); assert(!validationErrors);
        }
        std::puts("PASS: fully superseded writers skip copies, layer intersections retain exclusive texels");
    }

    void CheckMipGrowthDependencies(const vk::raii::Device &device,const vk::raii::PhysicalDevice &physical,u32 family) {
        SharedGuestMemory arena;
        GPU gpu{device,physical,family,true}; TextureManager manager{gpu};
        auto da{Describe(arena.guest+page)}; da.dimensions=vk::Extent3D{32,32,1};
        auto a{manager.FindOrCreate(da)}; WriteAliasRenderTarget(a->texture,{1,0,0,1});
        auto db{Describe(arena.guest+2*page)}; db.dimensions=vk::Extent3D{16,16,1};
        db.layerStride=1024; db.mappings={{arena.guest+2*page,1024}};
        auto b{manager.FindOrCreate(db)}; WriteAliasRenderTarget(b->texture,{0,1,0,1});
        // B occupies guest bytes not represented in A's original single mip.
        auto requested{da}; requested.mipLevelCount=requested.viewMipCount=3;
        auto grown{manager.FindOrCreate(requested)};
        assert(grown->texture==a->texture && a->texture->levelCount==3);
        std::vector<u8> expected(arena.observer+page,arena.observer+3*page);
        for (size_t i{};i<4096;++i) expected[i]=oldColor[i%4];
        for (size_t i{4096};i<5120;++i) expected[i]=newColor[i%4];
        VerifyAliasHost(a->texture,expected);
        WriteAliasRenderTarget(a->texture,{1,0,1,1}); // Existing added mips retain B's data.
        a->texture->SynchronizeGuest(false,false);
        for (size_t i{};i<4096;++i) assert(arena.observer[page+i]==pendingColor[i%4]);
        for (size_t i{4096};i<5120;++i) assert(arena.observer[page+i]==newColor[i%4]);
        CheckGuards(arena); gpu.scheduler.Complete(); assert(!validationErrors);
        std::puts("PASS: mip expansion resolves future texel dependencies and updates its footprint");
    }

}

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    using namespace skyline::gpu;
    vk::raii::Context context;
    const char *layer{"VK_LAYER_KHRONOS_validation"};
    const char *extensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME,VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    const vk::ValidationFeatureEnableEXT sync{vk::ValidationFeatureEnableEXT::eSynchronizationValidation};
    const vk::ValidationFeaturesEXT validation{.enabledValidationFeatureCount=1,.pEnabledValidationFeatures=&sync};
    const vk::ApplicationInfo app{.apiVersion=VK_API_VERSION_1_1};
    vk::raii::Instance instance{context,vk::InstanceCreateInfo{.pNext=&validation,.pApplicationInfo=&app,
        .enabledLayerCount=1,.ppEnabledLayerNames=&layer,.enabledExtensionCount=2,.ppEnabledExtensionNames=extensions}};
    vk::raii::DebugUtilsMessengerEXT debug{instance,vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity=vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType=vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation,.pfnUserCallback=ValidationMessage}};
    vk::raii::PhysicalDevice physical{std::move(instance.enumeratePhysicalDevices().front())};
    std::printf("Device: %s\n",physical.getProperties().deviceName.data());
    const auto queues{physical.getQueueFamilyProperties()};
    u32 family{}; while (!(queues.at(family).queueFlags & vk::QueueFlagBits::eGraphics)) ++family;
    const float priority{1};
    const vk::DeviceQueueCreateInfo queue{.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    const char *deviceExtensions[]{VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME};
    const vk::PhysicalDeviceImagelessFramebufferFeatures imageless{.imagelessFramebuffer=true};
    vk::raii::Device device{physical,vk::DeviceCreateInfo{.pNext=&imageless,.queueCreateInfoCount=1,
        .pQueueCreateInfos=&queue,.enabledExtensionCount=2,.ppEnabledExtensionNames=deviceExtensions}};
    if (std::getenv("TEXMAN_SUBRESOURCES_ONLY")) {
        CheckIndependentWriteValidation(device,physical,family);
        CheckCpuUploadWithPendingForeign(device,physical,family);
        for (bool mips : {false,true}) for (bool fragmented : {false,true}) for (bool free : {false,true})
            CheckViewOwnership(device,physical,family,mips,fragmented,free);
        return 0;
    }
    for (bool freeGuest : {false,true})
    for (bool reuseFirst : {false,true}) {
        CheckPartialAuthority(device,physical,family,false,true,true,reuseFirst,freeGuest);
        CheckPartialAuthority(device,physical,family,false,false,true,reuseFirst,freeGuest);
        for (bool preReadB : {false,true})
            for (bool trapWrite : {false,true}) CheckPartialAuthority(device,physical,family,true,preReadB,trapWrite,reuseFirst,freeGuest);
    }
    CheckPaddingAndFailure(device,physical,family);
    CheckExecutorRetry(device,physical,family);
    CheckExactPeers(device,physical,family);
    CheckFragmentedMappings(device,physical,family);
    CheckCachedDescriptorReuse(device,physical,family);
    CheckEvictedSampledOverlap(device,physical,family);
    CheckLateExactAlias(device,physical,family);
    CheckIndependentWriteValidation(device,physical,family);
    CheckCpuUploadWithPendingForeign(device,physical,family);
    for (bool mips : {false,true}) for (bool fragmented : {false,true}) for (bool free : {false,true})
        CheckViewOwnership(device,physical,family,mips,fragmented,free);
    CheckFullySupersededAndLayers(device,physical,family);
    CheckMipGrowthDependencies(device,physical,family);
    assert(!validationErrors);
    std::puts("PASS: partial overlap authority, live reuse, masked readback, B-Clean, A->B->A partial writes, exclusive texels, trap modes and steady-state copy counts; zero Vulkan validation errors");
}
