#include "context.h"
#include <gpu/pipeline_layout_cache.h>
#include <chrono>
#include <fstream>
#include <malloc.h>
#include <sstream>
#include <string>
#include <sched.h>

using skyline::gpu::PipelineLayoutCache;
struct Recipe {
    vk::DescriptorSetLayoutCreateFlags flags;
    vk::PipelineLayoutCreateFlags pipelineFlags;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    std::vector<vk::PushConstantRange> ranges;
    vk::DescriptorSetLayoutCreateInfo Info() const {
        return {.flags=flags,.bindingCount=static_cast<uint32_t>(bindings.size()),.pBindings=bindings.data()};
    }
};
struct MasterLayouts {
    vk::raii::DescriptorSetLayout descriptor;
    vk::raii::PipelineLayout pipeline;
    // Same create infos and inline ownership as e8e38237. No shared-owner or key
    // allocation is charged to the baseline; those are costs of the candidate.
    MasterLayouts(const vk::raii::Device &device,const Recipe &r)
        : descriptor{device,r.Info()},pipeline{device,vk::PipelineLayoutCreateInfo{
            .flags=r.pipelineFlags,.setLayoutCount=1,.pSetLayouts=&*descriptor,
            .pushConstantRangeCount=static_cast<uint32_t>(r.ranges.size()),.pPushConstantRanges=r.ranges.data()}} {}
};
static std::vector<Recipe> Load(const char *path) {
    std::ifstream file(path);if(!file)throw std::runtime_error("missing capture layout fixture");
    std::vector<Recipe> out;std::string line;
    while(std::getline(file,line)) {
        if(line.empty()||line[0]=='#')continue;
        std::istringstream s(line);uint32_t flags,pflags,nb,nr;s>>flags>>pflags>>nb>>nr;
        Recipe r{vk::DescriptorSetLayoutCreateFlags(flags),vk::PipelineLayoutCreateFlags(pflags),{}, {}};
        for(uint32_t i=0;i<nb;++i){uint32_t n,t,c,st;s>>n>>t>>c>>st;r.bindings.push_back({.binding=n,.descriptorType=vk::DescriptorType(t),.descriptorCount=c,.stageFlags=vk::ShaderStageFlags(st)});}
        for(uint32_t i=0;i<nr;++i){uint32_t st,o,n;s>>st>>o>>n;r.ranges.push_back({.stageFlags=vk::ShaderStageFlags(st),.offset=o,.size=n});}
        if(!s)throw std::runtime_error("invalid fixture row");out.push_back(std::move(r));
    }
    return out;
}
static int64_t Run(const vk::raii::Device &device,const std::vector<Recipe> &recipes,bool cached,int epochs,bool memory) {
    int64_t total=0;
    for(int i=0;i<epochs;++i) {
        PipelineLayoutCache cache(device);
        std::vector<MasterLayouts> masters;masters.reserve(recipes.size());
        std::vector<std::shared_ptr<const PipelineLayoutCache::Layouts>> owners;owners.reserve(recipes.size());
        auto before=mallinfo2().uordblks;
        auto start=std::chrono::steady_clock::now();
        for(const auto &r:recipes) {
            if(cached)owners.push_back(cache.Acquire(r.Info(),r.ranges,r.pipelineFlags));
            else masters.emplace_back(device,r);
        }
        total+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        if(memory)std::cout<<"{\"kind\":\"objects\",\"cached\":"<<(cached?"true":"false")
            <<",\"requests\":"<<recipes.size()<<",\"descriptor_creates\":"<<test::counts.sets
            <<",\"pipeline_layout_creates\":"<<test::counts.pipelines<<",\"driver_layout_bytes\":"<<test::counts.bytes
            <<",\"pipeline_cache_hits\":"<<(cached?recipes.size()-test::counts.pipelines.load():0)
            <<",\"descriptor_reuses\":"<<(cached?recipes.size()-test::counts.sets.load():0)
            <<",\"driver_allocation_calls\":"<<test::counts.allocations
            <<",\"heap_delta_bytes\":"<<static_cast<int64_t>(mallinfo2().uordblks)-static_cast<int64_t>(before)<<"}\n";
    }
    return total;
}
int main(int argc,char **argv) {
    try {
        if(argc!=2)throw std::runtime_error("usage: benchmark fixture.txt");
        auto recipes=Load(argv[1]);test::Context plain(false,false);
        test::Check(plain.device.getDispatcher()->vkCreateDescriptorSetLayout!=test::CreateSet,"timing device is instrumented");
        auto props=plain.physical.getProperties();
        std::cout<<"{\"kind\":\"device\",\"name\":\""<<props.deviceName.data()<<"\",\"driver_version\":"<<props.driverVersion<<",\"cpu\":"<<sched_getcpu()<<"}\n";
        for(bool cached:{false,true})Run(plain.device,recipes,cached,25,false);
        for(int round=0;round<11;++round)for(int pass=0;pass<2;++pass) {
            bool cached=pass==(round%2);
            auto ns=Run(plain.device,recipes,cached,300,false);
            std::cout<<"{\"kind\":\"timing\",\"round\":"<<round<<",\"cached\":"<<(cached?"true":"false")
                <<",\"epochs\":300,\"requests_per_epoch\":"<<recipes.size()<<",\"total_creation_ns\":"<<ns<<"}\n";
        }
        // Separate instrumented device: timing above has no layer, injected
        // allocation callbacks, interception, or counters on the Vulkan calls.
        test::Context measured(false,true);
        test::Check(measured.device.getDispatcher()->vkCreateDescriptorSetLayout==test::CreateSet,"object-count device is not instrumented");
        for(int round=0;round<5;++round)for(bool cached:{false,true}) {
            test::counts.Reset();Run(measured.device,recipes,cached,1,true);test::counts.Reset();
        }
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
