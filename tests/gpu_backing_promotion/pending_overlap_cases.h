// SPDX-License-Identifier: MPL-2.0
namespace skyline::gpu {
    void CheckPendingOverlaps(GPU &gpu) {
        for(bool freeGuest:{false,true}) {
            *gpu.state.settings->freeGuestTextureMemory=freeGuest;
            std::vector<u8> bytes(65536,0x23);
            auto writer=std::make_shared<Texture>(gpu,vk::Format::eR8G8B8A8Unorm,vk::Extent3D{128,128,1},1,1);
            writer->guest->layerStride=65536;writer->mirror=bytes;
            writer->guest->mappings.push_back(writer->mirror);
            writer->dirtyState=Texture::CpuDirty;
            auto pin=writer->PinGuestMemory(); // Keep this lifecycle check independent of discard policy.
            writer->SynchronizeHost(false);
            assert(writer->LockWithTag(71));
            writer->RecordGpuWrite();
            assert(writer->pendingGpuWriteTag==71);
            // A clean predecessor does not authorize reading while the new intent is queued.
            auto calls=gpu.scheduler.submissions;
            TextureAliasReconciliationRequest request;
            bool retried=false;
            try {TextureManager::SynchronizeGuestOverlaps({writer,writer},71);}
            catch(const TextureAliasReconciliationRequest &r){request=r;retried=true;}
            assert(retried && request.overlapReadbacks.size()==2);
            assert(gpu.scheduler.submissions==calls && writer->pendingGpuWriteTag==71);
            assert(std::all_of(bytes.begin(),bytes.end(),[](u8 x){return x==0x23;}));
            // Exception ownership holds the producer alive across unwind.
            std::weak_ptr<Texture> weak=writer;
            writer.reset();assert(!weak.expired());writer=weak.lock();
            // Production retry handler must submit before invoking the production readback helper.
            interconnect::CommandExecutor executor;
            executor.submitAction=[] { throw std::bad_alloc{}; };
            bool submitFailed=false;
            try { executor.HandleTextureBackingPromotion(request); }
            catch(const std::bad_alloc &) { submitFailed=true; }
            assert(submitFailed && writer->pendingGpuWriteTag==71 && gpu.scheduler.submissions==calls);
            executor.submissions=0;
            executor.submitAction=[&] {
                writer->unlock();
                WriteAliasRenderTarget(writer,{1,0,0,1});
                writer->dirtyState=Texture::GpuDirty;
            };
            executor.HandleTextureBackingPromotion(request);
            assert(executor.submissions==1 && writer->gpuReadbackSerial==writer->gpuWriteSerial);
            assert(bytes[0]==255 && bytes[1]==0 && bytes[2]==0 && bytes[3]==255);
            auto afterRetry=gpu.scheduler.submissions;
            TextureManager::SynchronizeGuestOverlaps({writer},71);
            assert(gpu.scheduler.submissions==afterRetry);
            assert(writer->LockWithTag(73));writer->RecordGpuWrite();
            writer->unlock(); // Production cancellation/unwind cleanup.
            assert(writer->pendingGpuWriteTag==0 && writer->tag==0);
            // Historical mismatch alone must not create an executor retry.
            writer->gpuWriteSerial=100;writer->gpuReadbackSerial=0;
            writer->dirtyState=Texture::CpuDirty;
            bytes[0]=0x42;
            TextureManager::SynchronizeGuestOverlaps({writer});
            assert(bytes[0]==0x42 && writer->dirtyState==Texture::CpuDirty);
            // A subsequent actual GPU producer writes after the old image contents.
            writer->SynchronizeHost(false);
            WriteAliasRenderTarget(writer,{0,1,0,1});
            assert(writer->pendingGpuWriteTag==0); // Untagged completed test producer.
            writer->cycle->Wait();
            writer->dirtyState=Texture::GpuDirty;
            writer->memoryFreed=true;
            writer->downloadStagingBuffer.reset();
            gpu.memory.failAllocation=true;
            bool failed=false;
            try{TextureManager::SynchronizeGuestOverlaps({writer});}catch(const std::bad_alloc&){failed=true;}
            gpu.memory.failAllocation=false;
            assert(failed && writer->dirtyState==Texture::GpuDirty && writer->memoryFreed);
            auto serial=writer->gpuWriteSerial;
            TextureManager::SynchronizeGuestOverlaps({writer,writer});
            assert(writer->gpuReadbackSerial==serial && !writer->memoryFreed && writer->dirtyState==Texture::Clean);
            std::vector<u8> expected(65536);
            for(size_t i=0;i<expected.size();++i)expected[i]=(i%4==1||i%4==3)?255:0;
            assert(bytes==expected);
            calls=gpu.scheduler.submissions;
            TextureManager::SynchronizeGuestOverlaps({writer});
            assert(gpu.scheduler.submissions==calls); // Clean retry adds no GPU work.
            // Tagged read-only ownership uses the production ContextLock and does not unlock the executor.
            assert(writer->LockWithTag(72));
            TextureManager::SynchronizeGuestOverlaps({writer},72);
            assert(writer->tag==72 && writer->pendingGpuWriteTag==0);
            writer->unlock();
            // Opposite discovery order must acquire the same sorted lock order.
            auto other=std::make_shared<Texture>(gpu,vk::Format::eR8G8B8A8Unorm);
            other->dirtyState=Texture::Clean;
            auto run=[&](bool reverse){for(unsigned n=0;n<100;++n)
                TextureManager::SynchronizeGuestOverlaps(reverse ? std::vector<std::shared_ptr<Texture>>{writer,other} :
                    std::vector<std::shared_ptr<Texture>>{other,writer});};
            auto first=std::async(std::launch::async,run,false);
            auto second=std::async(std::launch::async,run,true);
            assert(first.wait_for(std::chrono::seconds(5))==std::future_status::ready);
            assert(second.wait_for(std::chrono::seconds(5))==std::future_status::ready);
            first.get();second.get();
            request.overlapReadbacks.clear();pin.reset();
            gpu.scheduler.Complete();
            std::printf("PASS pending receipt/unwind/lifetime/CPU-newer/failed-readback/retry/no-extra-copy free=%u\n",unsigned(freeGuest));
        }
        *gpu.state.settings->freeGuestTextureMemory=false;
    }
}
