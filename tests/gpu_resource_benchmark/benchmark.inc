// SPDX-License-Identifier: MPL-2.0
// Injected only into the host fixture. Nothing here is production telemetry.
#include <sched.h>
namespace resource_bench {
    using ::resource_bench::allocations;
    using ::resource_bench::countAllocations;
    struct Counts { uint64_t copyCalls{},rectangles{},bytes{},barrierCalls{},barriers{}; } counts;
    bool countCommands{};
    PFN_vkCmdCopyImage imageCopy{};
    PFN_vkCmdCopyBufferToImage bufferImage{};
    PFN_vkCmdCopyImageToBuffer imageBuffer{};
    PFN_vkCmdCopyBuffer bufferCopy{};
    PFN_vkCmdPipelineBarrier pipelineBarrier{};
    void Count(uint32_t count,const VkImageCopy *regions){
        if(!countCommands)return;
        ++counts.copyCalls;counts.rectangles+=count;
        for(uint32_t i{};i<count;++i)counts.bytes+=uint64_t{regions[i].extent.width}*regions[i].extent.height*regions[i].extent.depth*regions[i].srcSubresource.layerCount*4;
    }
    void Count(uint32_t count,const VkBufferImageCopy *regions){
        if(!countCommands)return;
        ++counts.copyCalls;counts.rectangles+=count;
        for(uint32_t i{};i<count;++i)counts.bytes+=uint64_t{regions[i].imageExtent.width}*regions[i].imageExtent.height*regions[i].imageExtent.depth*regions[i].imageSubresource.layerCount*4;
    }
    VKAPI_ATTR void VKAPI_CALL CopyImage(VkCommandBuffer cmd,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageCopy *regions){Count(count,regions);imageCopy(cmd,src,sl,dst,dl,count,regions);}
    VKAPI_ATTR void VKAPI_CALL BufferImage(VkCommandBuffer cmd,VkBuffer src,VkImage dst,VkImageLayout layout,uint32_t count,const VkBufferImageCopy *regions){Count(count,regions);bufferImage(cmd,src,dst,layout,count,regions);}
    VKAPI_ATTR void VKAPI_CALL ImageBuffer(VkCommandBuffer cmd,VkImage src,VkImageLayout layout,VkBuffer dst,uint32_t count,const VkBufferImageCopy *regions){Count(count,regions);imageBuffer(cmd,src,layout,dst,count,regions);}
    VKAPI_ATTR void VKAPI_CALL CopyBuffer(VkCommandBuffer cmd,VkBuffer src,VkBuffer dst,uint32_t count,const VkBufferCopy *regions){
        if(countCommands){++counts.copyCalls;counts.rectangles+=count;for(uint32_t i{};i<count;++i)counts.bytes+=regions[i].size;}
        bufferCopy(cmd,src,dst,count,regions);
    }
    VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,uint32_t memoryCount,const VkMemoryBarrier *memory,uint32_t bufferCount,const VkBufferMemoryBarrier *buffers,uint32_t imageCount,const VkImageMemoryBarrier *images){
        if(countCommands){++counts.barrierCalls;counts.barriers+=memoryCount+bufferCount+imageCount;}
        pipelineBarrier(cmd,src,dst,flags,memoryCount,memory,bufferCount,buffers,imageCount,images);
    }
    template<class Function> void Measure(const char *name,bool formatList,Function &&operation){
        constexpr unsigned iterations{10000};
        for(unsigned i{};i<1000;++i)operation();
        std::array<double,7> samples;
        for(auto &sample:samples){
            const auto start=std::chrono::steady_clock::now();
            for(unsigned i{};i<iterations;++i)operation();
            sample=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
        }
        allocations=0;countAllocations=true;counts={};countCommands=true;
        for(unsigned i{};i<1000;++i)operation();
        countCommands=false;countAllocations=false;
        std::sort(samples.begin(),samples.end());
        std::printf("BENCH_JSON {\"case\":\"%s\",\"format_list\":%s,\"iterations\":%u,\"median_ns\":%.3f,\"p90_ns\":%.3f,\"allocations_per_1000\":%llu,\"copy_calls_per_1000\":%llu,\"barrier_calls_per_1000\":%llu}\n",name,formatList?"true":"false",iterations,samples[3],samples[6],allocations,(unsigned long long)counts.copyCalls,(unsigned long long)counts.barrierCalls);
    }
}
void CheckResourceBenchmark(GPU &gpu,bool formatList){
    using namespace resource_bench;
    if (const auto *cpu=std::getenv("TEXMAN_BENCH_CPU")) {
        cpu_set_t affinity;
        CPU_ZERO(&affinity);
        CPU_SET(std::stoi(cpu), &affinity);
        if (sched_setaffinity(0, sizeof(affinity), &affinity))
            throw std::runtime_error("Cannot pin benchmark control thread");
    }
    auto *dispatcher=const_cast<vk::raii::DeviceDispatcher *>(gpu.vkDevice.getDispatcher());
    imageCopy=dispatcher->vkCmdCopyImage;bufferImage=dispatcher->vkCmdCopyBufferToImage;
    imageBuffer=dispatcher->vkCmdCopyImageToBuffer;bufferCopy=dispatcher->vkCmdCopyBuffer;pipelineBarrier=dispatcher->vkCmdPipelineBarrier;
    dispatcher->vkCmdCopyImage=CopyImage;dispatcher->vkCmdCopyBufferToImage=BufferImage;
    dispatcher->vkCmdCopyImageToBuffer=ImageBuffer;dispatcher->vkCmdCopyBuffer=CopyBuffer;dispatcher->vkCmdPipelineBarrier=Barrier;
    {
        std::vector<u8> bytes(6*4096,0x2D);TextureManager manager{gpu};
        auto plain{manager.FindOrCreate(PartialDescriptor(bytes.data()))};
        AliasRead(plain->texture);
        Measure("unshared_image_writer",formatList,[&]{plain->texture->RecordGpuWrite();});
        {
            interconnect::CommandExecutor unshared;plain->lockOnAttach=true;
            std::array<TextureView *,1> writes{plain.get()};
            Measure("unshared_image_prepare_commit",formatList,[&]{unshared.RunWithTextureBackingRetry([&]{unshared.AttachTexture(plain.get());unshared.CommitTextureWrites(writes);});});
            for(auto &resource:unshared.attachedTextures)resource->unlock();unshared.attachedTextures.clear();
        }
        auto a{manager.FindOrCreate(PartialDescriptor(bytes.data()+3*4096))};
        auto b{manager.FindOrCreate(PartialDescriptor(bytes.data()+4*4096))};
        AliasRead(a->texture);AliasRead(b->texture);
        a->texture->RecordGpuWrite();
        Measure("shared_image_writer",formatList,[&]{a->texture->RecordGpuWrite();});
        Measure("valid_region_reconcile",formatList,[&]{a->texture->ReconcileRegions();});
        interconnect::CommandExecutor executor;a->lockOnAttach=true;
        std::array<TextureView *,1> writers{a.get()};
        auto commit=[&]{executor.RunWithTextureBackingRetry([&]{executor.AttachTexture(a.get());executor.CommitTextureWrites(writers);});};
        Measure("shared_image_prepare_commit",formatList,commit);
        for(auto &resource:executor.attachedTextures)resource->unlock();executor.attachedTextures.clear();
        WriteAliasRenderTarget(b->texture,{0,1,0,1});
        auto waits=[&]{uint64_t submit{},gpuWait{};for(const auto &cycle:gpu.scheduler.cycles){submit+=cycle->submitWaits;gpuWait+=cycle->gpuWaits;}return std::pair{submit,gpuWait};};
        const auto before=waits();const auto submissions=gpu.scheduler.submissions;
        counts={};countCommands=true;
        const auto start=std::chrono::steady_clock::now();AliasRead(a->texture);
        const auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
        countCommands=false;const auto after=waits();
        std::printf("BENCH_JSON {\"case\":\"modified_image_region_repair\",\"format_list\":%s,\"record_ns\":%.3f,\"bytes\":%llu,\"copy_calls\":%llu,\"rectangles\":%llu,\"barrier_calls\":%llu,\"barriers\":%llu,\"submissions\":%u,\"submit_wait_calls\":%llu,\"gpu_completion_wait_calls\":%llu}\n",formatList?"true":"false",elapsed,(unsigned long long)counts.bytes,(unsigned long long)counts.copyCalls,(unsigned long long)counts.rectangles,(unsigned long long)counts.barrierCalls,(unsigned long long)counts.barriers,gpu.scheduler.submissions-submissions,(unsigned long long)(after.first-before.first),(unsigned long long)(after.second-before.second));
        assert(counts.bytes==4096 && after.second==before.second);
        const auto data=DownloadPartial(a->texture);
        for(size_t i{4096};i<data.size();++i)assert((data[i]==std::array<u8,4>{0,255,0,255}[i%4]));
        gpu.scheduler.Complete();assert(!validationErrors);
    }
    dispatcher->vkCmdCopyImage=imageCopy;dispatcher->vkCmdCopyBufferToImage=bufferImage;
    dispatcher->vkCmdCopyImageToBuffer=imageBuffer;dispatcher->vkCmdCopyBuffer=bufferCopy;dispatcher->vkCmdPipelineBarrier=pipelineBarrier;
}
