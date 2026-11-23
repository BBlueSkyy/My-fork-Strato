#pragma once
#include "integration_facades.h"
#include <filesystem>
#include <fstream>
#include <optional>

namespace execution {
    using skyline::gpu::GPU;
    using skyline::gpu::GraphicsPipelineAssembler;
    using skyline::gpu::DescriptorAllocator;
    using test::Check;

    inline uint32_t MemoryType(test::Context &v,uint32_t bits,vk::MemoryPropertyFlags flags) {
        auto props=v.physical.getMemoryProperties();
        for(uint32_t i=0;i<props.memoryTypeCount;++i)
            if((bits&(1U<<i))&&(props.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("required Vulkan memory type unavailable");
    }
    struct CpuBuffer {
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Buffer buffer;
        uint32_t *data{};
        CpuBuffer(test::Context &v,size_t bytes)
            : buffer{v.device,vk::BufferCreateInfo{.size=bytes,.usage=vk::BufferUsageFlagBits::eStorageBuffer|vk::BufferUsageFlagBits::eTransferDst}} {
            auto r=buffer.getMemoryRequirements();
            memory=vk::raii::DeviceMemory{v.device,vk::MemoryAllocateInfo{.allocationSize=r.size,
                .memoryTypeIndex=MemoryType(v,r.memoryTypeBits,vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent)}};
            buffer.bindMemory(*memory,0);data=static_cast<uint32_t *>(memory.mapMemory(0,VK_WHOLE_SIZE));
        }
        ~CpuBuffer(){memory.unmapMemory();}
    };
    inline vk::raii::ShaderModule Shader(test::Context &v,const std::filesystem::path &path) {
        std::ifstream s(path,std::ios::binary|std::ios::ate);if(!s)throw std::runtime_error("missing test SPIR-V");
        size_t bytes=static_cast<size_t>(s.tellg());std::vector<uint32_t> code(bytes/4);
        s.seekg(0);s.read(reinterpret_cast<char *>(code.data()),bytes);
        return vk::raii::ShaderModule{v.device,vk::ShaderModuleCreateInfo{.codeSize=bytes,.pCode=code.data()}};
    }
    template<class F> void Submit(test::Context &v,F record) {
        vk::raii::CommandPool pool{v.device,vk::CommandPoolCreateInfo{.queueFamilyIndex=v.family}};
        auto buffers=v.device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1});
        auto &cmd=buffers[0];cmd.begin(vk::CommandBufferBeginInfo{});record(cmd);cmd.end();
        vk::raii::Fence fence{v.device,vk::FenceCreateInfo{}};
        auto raw=*cmd;
        v.device.getQueue(v.family,0).submit(vk::SubmitInfo{.commandBufferCount=1,.pCommandBuffers=&raw},*fence);
        Check(v.device.waitForFences(*fence,true,UINT64_MAX)==vk::Result::eSuccess,"submission fence failed");
    }
    inline void HostBarrier(vk::raii::CommandBuffer &cmd,vk::PipelineStageFlags stage,vk::AccessFlags access) {
        vk::MemoryBarrier b{.srcAccessMask=access,.dstAccessMask=vk::AccessFlagBits::eHostRead};
        cmd.pipelineBarrier(stage,vk::PipelineStageFlagBits::eHost,{},b,{},{});
    }
    inline void Compute(test::Context &v,const std::filesystem::path &build) {
        using namespace skyline::gpu::interconnect::kepler_compute;
        skyline::gpu::TestJvm jvm;GPU gpu(v.device,jvm);GraphicsPipelineAssembler assembler(gpu,"");gpu.graphicsPipelineAssembler=&assembler;
        DescriptorAllocator descriptors(gpu);
        auto shader1=Shader(v,build/"compute.spv"),shader3=Shader(v,build/"compute3.spv");
        std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
            {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
            {.binding=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}}};
        InterconnectContext ctx{gpu};
        auto a=MakeCompiledPipeline(ctx,{},Pipeline::ShaderStage{*shader1},bindings);
        auto b=MakeCompiledPipeline(ctx,{},Pipeline::ShaderStage{*shader3},bindings);
        Check(a.layouts==b.layouts,"production compute did not reuse layouts");
        Check(a.pipelineLayout==b.pipelineLayout&&a.descriptorSetLayout==b.descriptorSetLayout,"compute borrowed handles differ");
        CpuBuffer out(v,4),input(v,8);input.data[0]=0xdeadbeef;input.data[1]=0x01020304;
        std::array<vk::DescriptorBufferInfo,2> buffers{{{.buffer=*out.buffer,.range=4},{.buffer=*input.buffer,.range=8}}};
        auto writes=[&](vk::DescriptorSet set){return std::array<vk::WriteDescriptorSet,2>{{
            {.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffers[0]},
            {.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffers[1]}}};};
        std::optional<DescriptorAllocator::ActiveDescriptorSet> active{descriptors.AllocateSet(a.descriptorSetLayout)};
        vk::DescriptorSet first=**active;
        auto simultaneous=descriptors.AllocateSet(b.descriptorSetLayout);
        Check(*simultaneous!=first,"an active descriptor slot was reused");
        v.device.updateDescriptorSets(writes(first),{});
        Submit(v,[&](auto &cmd){
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*a.pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,a.pipelineLayout,0,first,{});cmd.dispatch(1,1,1);
            vk::MemoryBarrier barrier{.srcAccessMask=vk::AccessFlagBits::eShaderWrite,.dstAccessMask=vk::AccessFlagBits::eShaderWrite};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eComputeShader,{},barrier,{},{});
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*b.pipeline);cmd.dispatch(1,1,1);
            HostBarrier(cmd,vk::PipelineStageFlagBits::eComputeShader,vk::AccessFlagBits::eShaderWrite);
        });
        Check(*out.data==(input.data[0]+input.data[1])*3U,"shared layout changed shader output or integer bits");
        active.reset();
        auto reused=descriptors.AllocateSet(b.descriptorSetLayout);
        Check(*reused==first,"production allocator did not reuse the completed compatible slot");
        if(v.push) {
            gpu.traits.push=true;
            auto p=MakeCompiledPipeline(ctx,{},Pipeline::ShaderStage{*shader1},bindings);
            Check(p.usesPushDescriptors&&p.layouts!=a.layouts,"production push policy was merged with ordinary sets");
            Submit(v,[&](auto &cmd){
                auto updates=writes({});cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*p.pipeline);
                cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,p.pipelineLayout,0,updates);cmd.dispatch(1,1,1);
                HostBarrier(cmd,vk::PipelineStageFlagBits::eComputeShader,vk::AccessFlagBits::eShaderWrite);
            });
            Check(*out.data==input.data[0]+input.data[1],"push-descriptor compute output differs");
        }
    }

    inline void Graphics(test::Context &v,const std::filesystem::path &build) {
        skyline::gpu::TestJvm jvm;GPU gpu(v.device,jvm);GraphicsPipelineAssembler assembler(gpu,"");gpu.graphicsPipelineAssembler=&assembler;
        DescriptorAllocator descriptors(gpu);
        auto vs=Shader(v,build/"vertex.spv"),fs=Shader(v,build/"fragment.spv");
        std::array<vk::PipelineShaderStageCreateInfo,2> stages{{
            {.stage=vk::ShaderStageFlagBits::eVertex,.module=*vs,.pName="main"},
            {.stage=vk::ShaderStageFlagBits::eFragment,.module=*fs,.pName="main"}}};
        vk::StructureChain<vk::PipelineVertexInputStateCreateInfo,vk::PipelineVertexInputDivisorStateCreateInfoEXT> vertex;
        vertex.unlink<vk::PipelineVertexInputDivisorStateCreateInfoEXT>();
        vk::StructureChain<vk::PipelineRasterizationStateCreateInfo,vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT> raster;
        raster.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
        raster.get<vk::PipelineRasterizationStateCreateInfo>().lineWidth=1;
        vk::PipelineInputAssemblyStateCreateInfo assembly{.topology=vk::PrimitiveTopology::eTriangleList};
        vk::PipelineTessellationStateCreateInfo tessellation{};
        vk::Viewport viewport{.width=4,.height=4,.maxDepth=1};vk::Rect2D scissor{.extent={4,4}};
        vk::PipelineViewportStateCreateInfo viewportState{.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
        vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples=vk::SampleCountFlagBits::e1};
        vk::PipelineDepthStencilStateCreateInfo depth{};
        vk::PipelineColorBlendAttachmentState blend{.colorWriteMask=vk::ColorComponentFlagBits::eR};
        vk::PipelineColorBlendStateCreateInfo color{.attachmentCount=1,.pAttachments=&blend};
        vk::PipelineDynamicStateCreateInfo dynamic{};vk::Format format=vk::Format::eR32Uint;
        GraphicsPipelineAssembler::PipelineState state{stages,vertex,assembly,tessellation,viewportState,raster,multisample,depth,color,dynamic,{&format,1},vk::Format::eUndefined,vk::SampleCountFlagBits::e1,false};
        const auto stagesMask=vk::ShaderStageFlagBits::eFragment|vk::ShaderStageFlagBits::eCompute;
        std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
            {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=stagesMask},
            {.binding=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=stagesMask}}};
        vk::PushConstantRange push{.stageFlags=vk::ShaderStageFlagBits::eFragment,.size=4};
        auto a=assembler.AssemblePipelineAsync(state,bindings,{&push,1},true);
        raster.get<vk::PipelineRasterizationStateCreateInfo>().frontFace=vk::FrontFace::eClockwise;
        auto b=assembler.AssemblePipelineAsync(state,bindings,{&push,1},true);
        Check(a.layouts==b.layouts,"production graphics async path duplicated layouts");
        assembler.WaitIdle();
        CpuBuffer input(v,4),computeInput(v,8),readback(v,64);
        computeInput.data[0]=0x12345670;computeInput.data[1]=8;
        auto set=descriptors.AllocateSet(a.descriptorSetLayout);
        std::array<vk::DescriptorBufferInfo,2> bi{{{.buffer=*input.buffer,.range=4},{.buffer=*computeInput.buffer,.range=8}}};
        std::array<vk::WriteDescriptorSet,2> updates{{
            {.dstSet=*set,.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&bi[0]},
            {.dstSet=*set,.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&bi[1]}}};
        v.device.updateDescriptorSets(updates,{});
        // Same descriptor interface crosses production graphics/compute builders.
        // Graphics has push constants; compute must get a distinct pipeline layout
        // backed by the same descriptor owner, without invalidating the set.
        namespace compute=skyline::gpu::interconnect::kepler_compute;
        auto cs=Shader(v,build/"compute.spv");compute::InterconnectContext ctx{gpu};
        auto c=compute::MakeCompiledPipeline(ctx,{},compute::Pipeline::ShaderStage{*cs},bindings);
        Check(c.descriptorSetLayout==a.descriptorSetLayout,"graphics/compute descriptor layout did not share");
        Check(c.pipelineLayout!=a.pipelineLayout,"graphics push constants leaked into compute layout");
        Submit(v,[&](auto &cmd){
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*c.pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,c.pipelineLayout,0,*set,{});cmd.dispatch(1,1,1);
            HostBarrier(cmd,vk::PipelineStageFlagBits::eComputeShader,vk::AccessFlagBits::eShaderWrite);
        });
        Check(*input.data==0x12345678,"shared graphics/compute set changed compute output");
        vk::raii::DeviceMemory memory{nullptr};
        vk::raii::Image image{v.device,vk::ImageCreateInfo{.imageType=vk::ImageType::e2D,.format=format,.extent={4,4,1},.mipLevels=1,.arrayLayers=1,
            .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc}};
        auto req=image.getMemoryRequirements();memory=vk::raii::DeviceMemory{v.device,vk::MemoryAllocateInfo{.allocationSize=req.size,.memoryTypeIndex=MemoryType(v,req.memoryTypeBits,{})}};image.bindMemory(*memory,0);
        vk::ImageSubresourceRange range{.aspectMask=vk::ImageAspectFlagBits::eColor,.levelCount=1,.layerCount=1};
        vk::raii::ImageView view{v.device,vk::ImageViewCreateInfo{.image=*image,.viewType=vk::ImageViewType::e2D,.format=format,.subresourceRange=range}};
        vk::AttachmentDescription attachment{.flags=vk::AttachmentDescriptionFlagBits::eMayAlias,.format=format,.samples=vk::SampleCountFlagBits::e1,.loadOp=vk::AttachmentLoadOp::eDontCare,.storeOp=vk::AttachmentStoreOp::eStore,
            .initialLayout=vk::ImageLayout::eGeneral,.finalLayout=vk::ImageLayout::eGeneral};
        vk::AttachmentReference reference{.attachment=0,.layout=vk::ImageLayout::eGeneral};
        vk::SubpassDescription subpass{.pipelineBindPoint=vk::PipelineBindPoint::eGraphics,.colorAttachmentCount=1,.pColorAttachments=&reference};
        vk::raii::RenderPass pass{v.device,vk::RenderPassCreateInfo{.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass}};
        auto viewHandle=*view;
        vk::raii::Framebuffer frame{v.device,vk::FramebufferCreateInfo{.renderPass=*pass,.attachmentCount=1,.pAttachments=&viewHandle,.width=4,.height=4,.layers=1}};
        for(uint32_t run=0;run<2;++run) {
            Submit(v,[&](auto &cmd){
                vk::ImageMemoryBarrier before{.srcAccessMask=run?vk::AccessFlagBits::eTransferRead:vk::AccessFlags{},.dstAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,
                    .oldLayout=run?vk::ImageLayout::eGeneral:vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=*image,.subresourceRange=range};
                cmd.pipelineBarrier(run?vk::PipelineStageFlagBits::eTransfer:vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eColorAttachmentOutput,{}, {},{},before);
                cmd.beginRenderPass(vk::RenderPassBeginInfo{.renderPass=*pass,.framebuffer=*frame,.renderArea={.extent={4,4}}},vk::SubpassContents::eInline);
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*a.pipeline.get());
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,a.pipelineLayout,0,*set,{});
                uint32_t value=3;cmd.template pushConstants<uint32_t>(a.pipelineLayout,vk::ShaderStageFlagBits::eFragment,0,value);cmd.draw(3,1,0,0);
                if(run) {
                    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*b.pipeline.get());
                    value=7;cmd.template pushConstants<uint32_t>(b.pipelineLayout,vk::ShaderStageFlagBits::eFragment,0,value);cmd.draw(3,1,0,0);
                }
                cmd.endRenderPass();
                vk::ImageMemoryBarrier after{.srcAccessMask=vk::AccessFlagBits::eColorAttachmentWrite,.dstAccessMask=vk::AccessFlagBits::eTransferRead,
                    .oldLayout=vk::ImageLayout::eGeneral,.newLayout=vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=*image,.subresourceRange=range};
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,vk::PipelineStageFlagBits::eTransfer,{}, {},{},after);
                vk::BufferImageCopy copy{.imageSubresource={.aspectMask=vk::ImageAspectFlagBits::eColor,.layerCount=1},.imageExtent={4,4,1}};
                cmd.copyImageToBuffer(*image,vk::ImageLayout::eGeneral,*readback.buffer,copy);
                HostBarrier(cmd,vk::PipelineStageFlagBits::eTransfer,vk::AccessFlagBits::eTransferWrite);
            });
            for(unsigned pixel=0;pixel<16;++pixel)Check(readback.data[pixel]==*input.data+(run?7U:3U),"shared graphics layout changed pixels/push constants");
        }
    }
    inline void DeferredSecondaryCompilation(test::Context &v,const std::filesystem::path &build) {
        auto shader=Shader(v,build/"compute.spv");
        auto cache=std::make_unique<skyline::gpu::PipelineLayoutCache>(v.device);
        std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
            {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
            {.binding=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}}};
        vk::DescriptorSetLayoutCreateInfo info{.bindingCount=2,.pBindings=bindings.data()};
        auto primary=cache->Acquire(info);
        vk::PushConstantRange range{.stageFlags=vk::ShaderStageFlagBits::eCompute,.size=4};
        auto secondary=cache->Acquire(info,{&range,1});
        std::weak_ptr<const skyline::gpu::PipelineLayoutCache::Layouts> root=primary;
        std::promise<void> release;auto gate=release.get_future();
        auto pending=std::async(std::launch::async,[&,owner=secondary,gate=std::move(gate)]() mutable {
            gate.wait();
            vk::PipelineShaderStageCreateInfo stage{.stage=vk::ShaderStageFlagBits::eCompute,.module=*shader,.pName="main"};
            return vk::raii::Pipeline{v.device,nullptr,vk::ComputePipelineCreateInfo{.stage=stage,.layout=*owner->pipeline}};
        });
        cache.reset();primary.reset();secondary.reset();
        Check(!root.expired(),"pending secondary compile lost descriptor owner");
        release.set_value();
        { auto compiled=pending.get();Check(static_cast<bool>(*compiled),"compile after cache teardown failed"); }
        pending={};Check(root.expired(),"pending secondary compile leaked root owner");
    }
    inline void DeferredOwner(test::Context &v) {
        auto before=test::counts.pipelineDestroyed.load();
        auto cache=std::make_unique<skyline::gpu::PipelineLayoutCache>(v.device);
        auto owner=cache->Acquire(vk::DescriptorSetLayoutCreateInfo{});
        std::promise<void> release;auto gate=release.get_future();
        auto pending=std::async(std::launch::async,[owner,gate=std::move(gate)]() mutable {gate.wait();return owner->GetDescriptorSetLayout();});
        cache.reset();owner.reset();
        Check(test::counts.pipelineDestroyed==before,"pending task lost layout ownership at cache teardown");
        release.set_value();Check(static_cast<bool>(pending.get()),"pending owner became invalid");
        // std::async's shared state is allowed to retain the callable until the
        // future is destroyed; destroy it before checking destruction counts.
        pending={};
        Check(test::counts.pipelineDestroyed==before+1,"deferred layout owner leaked");
    }
}
