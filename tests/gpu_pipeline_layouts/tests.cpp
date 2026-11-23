#include "context.h"
#include <gpu/pipeline_layout_cache.h>
#include <array>
#include <future>
#include <thread>
#include <barrier>
#include "execution.h"

using skyline::gpu::PipelineLayoutCache;
using test::Check;
static void Keys(test::Context &v) {
    PipelineLayoutCache cache(v.device);
    std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
        {.binding=0,.descriptorType=vk::DescriptorType::eUniformBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eVertex},
        {.binding=2,.descriptorType=vk::DescriptorType::eCombinedImageSampler,.descriptorCount=2,.stageFlags=vk::ShaderStageFlagBits::eFragment}}};
    auto info=[&]{return vk::DescriptorSetLayoutCreateInfo{.bindingCount=2,.pBindings=bindings.data()};};
    auto a=cache.Acquire(info());
    Check(a==cache.Acquire(info()),"identical layout not reused");
    auto copy=bindings;auto moved=info();moved.pBindings=copy.data();
    Check(a==cache.Acquire(moved),"binding array pointer is not structural state");
    std::swap(bindings[0],bindings[1]);
    Check(a==cache.Acquire(info()),"binding order changes an extension-free interface");
    std::swap(bindings[0],bindings[1]);
    for(int kind=0;kind<4;++kind) {
        auto changed=bindings;
        if(kind==0)changed[0].binding=1;
        if(kind==1)changed[0].descriptorType=vk::DescriptorType::eStorageBuffer;
        if(kind==2)changed[0].descriptorCount=2;
        if(kind==3)changed[0].stageFlags=vk::ShaderStageFlagBits::eFragment;
        moved=info();moved.pBindings=changed.data();
        Check(a!=cache.Acquire(moved),"binding difference collapsed");
    }
    vk::PushConstantRange range{.stageFlags=vk::ShaderStageFlagBits::eVertex,.offset=0,.size=8};
    auto b=cache.Acquire(info(),{&range,1});
    Check(a!=b,"push constants missing from pipeline key");
    Check(a->descriptor==b->descriptor,"different push constants duplicated the descriptor layout");
    for(int kind=0;kind<3;++kind) {
        auto r=range;
        if(kind==0)r.offset=4;
        if(kind==1)r.size=12;
        if(kind==2)r.stageFlags=vk::ShaderStageFlagBits::eFragment;
        Check(b!=cache.Acquire(info(),{&r,1}),"push range difference collapsed");
    }
    if(v.push) {
        auto pushed=info();pushed.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
        auto p=cache.Acquire(pushed);Check(p!=a,"push and ordinary layouts merged");
        Check(p==cache.Acquire(pushed),"push layout not interned");
    }
    vk::DescriptorBindingFlags flags[2]{};
    vk::DescriptorSetLayoutBindingFlagsCreateInfo extension{.bindingCount=2,.pBindingFlags=flags};
    auto extended=info();extended.pNext=&extension;
    auto x=cache.Acquire(extended),y=cache.Acquire(extended);
    Check(x!=y&&x->descriptor!=y->descriptor,"pNext state must bypass interning");
    auto sampler=v.device.createSampler(vk::SamplerCreateInfo{.maxLod=1});
    std::array<vk::Sampler,2> samplers{*sampler,*sampler};
    auto immutable=bindings;immutable[1].pImmutableSamplers=samplers.data();
    moved=info();moved.pBindings=immutable.data();
    auto i=cache.Acquire(moved),j=cache.Acquire(moved);
    Check(i!=j&&i->descriptor!=j->descriptor,"immutable samplers must not enter a device-lifetime cache");
    Check(cache.Acquire(info())==a,"bypass polluted the plain key");
    // Future flags/pNext must not match the plain key. Reject their creates in
    // the dispatch shim before the driver/VVL: no invalid Vulkan call is issued.
    auto unknown=info();unknown.flags=vk::DescriptorSetLayoutCreateFlags(0x40000000U);
    test::counts.failSet=1;bool failed=false;
    try{cache.Acquire(unknown);}catch(const vk::OutOfHostMemoryError&){failed=true;}
    Check(failed&&test::counts.seenSetFlags==0x40000000U,"set flags were dropped from lookup or creation");
    test::counts.failPipeline=1;failed=false;
    try{cache.Acquire(info(),{},vk::PipelineLayoutCreateFlags(0x40000000U));}catch(const vk::OutOfHostMemoryError&){failed=true;}
    Check(failed&&test::counts.seenPipelineFlags==0x40000000U,"pipeline flags were dropped from lookup or creation");
    int sentinel{};test::counts.failPipeline=1;failed=false;
    try{cache.Acquire(info(),{}, {},&sentinel);}catch(const vk::OutOfHostMemoryError&){failed=true;}
    Check(failed&&test::counts.seenPipelineNext==&sentinel,"pipeline extension chain was cached or dropped");
    for(uint32_t n=0;n<160;++n) {
        vk::DescriptorSetLayoutBinding varied{.binding=n,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute};
        cache.Acquire(vk::DescriptorSetLayoutCreateInfo{.bindingCount=1,.pBindings=&varied});
        Check(cache.Acquire(info())==a,"rehash invalidated the canonical descriptor-key address");
    }
}
static void ConcurrentAndFailure(test::Context &v) {
    PipelineLayoutCache cache(v.device);
    vk::DescriptorSetLayoutCreateInfo info{};
    auto s=test::counts.sets.load(),p=test::counts.pipelines.load();
    std::vector<std::future<std::shared_ptr<const PipelineLayoutCache::Layouts>>> tasks;
    std::barrier start(25);
    for(int n=0;n<24;++n)tasks.push_back(std::async(std::launch::async,[&]{start.arrive_and_wait();return cache.Acquire(info);}));
    start.arrive_and_wait();
    auto first=tasks.front().get();
    for(size_t n=1;n<tasks.size();++n)Check(tasks[n].get()==first,"concurrent duplicate published");
    Check(test::counts.sets==s+1&&test::counts.pipelines==p+1,"concurrent creation was duplicated");
    vk::PushConstantRange range{.stageFlags=vk::ShaderStageFlagBits::eCompute,.size=4};
    test::counts.failPipeline=1;bool failed=false;
    try{cache.Acquire(info,{&range,1});}catch(const vk::OutOfHostMemoryError&){failed=true;}
    Check(failed,"pipeline creation error was swallowed");
    auto next=cache.Acquire(info,{&range,1});
    Check(next!=first&&next->descriptor==first->descriptor,"pipeline failure poisoned or discarded the set entry");
    PipelineLayoutCache other(v.device);test::counts.failSet=1;failed=false;
    try{other.Acquire(info);}catch(const vk::OutOfHostMemoryError&){failed=true;}
    Check(failed,"descriptor creation error was swallowed");
    Check(other.Acquire(info)!=first,"independent cache shared ownership unexpectedly");
}
static void DeviceIsolation(test::Context &v) {
    PipelineLayoutCache first(v.device);
    auto owner=first.Acquire(vk::DescriptorSetLayoutCreateInfo{});
    {
        test::Context other(false);
        PipelineLayoutCache second(other.device);
        auto foreign=second.Acquire(vk::DescriptorSetLayoutCreateInfo{});
        Check(*other.device!=*v.device,"device isolation fixture did not create another device");
        Check(owner!=foreign,"layout cache leaked across logical devices");
    }
    Check(owner==first.Acquire(vk::DescriptorSetLayoutCreateInfo{}),"other-device teardown invalidated the original cache");
}
int main(int argc,char **argv) {
    try {
        test::Context context;
        Keys(context);test::counts.Reset();
        ConcurrentAndFailure(context);test::counts.Reset();
        if(argc!=2)throw std::runtime_error("SPIR-V build directory required");
        execution::Compute(context,argv[1]);test::counts.Reset();
        execution::Graphics(context,argv[1]);test::counts.Reset();
        execution::DeferredOwner(context);test::counts.Reset();
        DeviceIsolation(context);test::counts.Reset();
        Check(test::counts.errors==0,"Vulkan validation errors");
        std::cout<<"PASS "<<test::checks<<" checks; zero validation errors\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
