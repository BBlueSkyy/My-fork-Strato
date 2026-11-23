#include "context.h"
#include <gpu/pipeline_layout_cache.h>
#include <chrono>
#include <fstream>
#include <malloc.h>
#include <sstream>
#include <string>
#include <sched.h>
#if __has_include("previous-layout-cache.h")
#include "previous-layout-cache.h"
#include "unlocked-layout-cache.h"
#define LAYOUT_REVIEW 1
#endif
#ifdef LAYOUT_DIAGNOSTICS
#include "allocation_probe.h"
#endif

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
struct Timing { int64_t creation{}, lifecycle{}; };

template<class Cache>
static Timing Run(const vk::raii::Device &device,const std::vector<Recipe> &recipes,
                  const char *name,bool cached,int epochs,bool memory) {
    Timing total;
    for(int i=0;i<epochs;++i) {
        auto epochStart=std::chrono::steady_clock::now();
        {
            Cache cache(device);
            std::vector<MasterLayouts> masters;masters.reserve(recipes.size());
            std::vector<std::shared_ptr<const typename Cache::Layouts>> owners;owners.reserve(recipes.size());
            [[maybe_unused]] auto before=memory?mallinfo2().uordblks:0;
#ifdef LAYOUT_DIAGNOSTICS
            allocation_probe::calls=0;allocation_probe::bytes=0;allocation_probe::enabled=memory;
#endif
            auto start=std::chrono::steady_clock::now();
            for(const auto &r:recipes) {
                if(cached)owners.push_back(cache.Acquire(r.Info(),r.ranges,r.pipelineFlags));
                else masters.emplace_back(device,r);
            }
            total.creation+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
#ifdef LAYOUT_DIAGNOSTICS
            allocation_probe::enabled=false;
            if(memory)std::cout<<"{\"kind\":\"objects\",\"variant\":\""<<name<<"\",\"requests\":"<<recipes.size()
                <<",\"descriptor_creates\":"<<test::counts.sets<<",\"pipeline_layout_creates\":"<<test::counts.pipelines
                <<",\"pipeline_layout_cache_hits\":"<<(cached?recipes.size()-test::counts.pipelines.load():0)
                <<",\"descriptor_reuses\":"<<(cached?recipes.size()-test::counts.sets.load():0)
                <<",\"cpp_new_calls\":"<<allocation_probe::calls<<",\"cpp_new_bytes\":"<<allocation_probe::bytes
                <<",\"heap_delta_bytes\":"<<static_cast<int64_t>(mallinfo2().uordblks)-static_cast<int64_t>(before)<<"}\n";
#endif
        }
        total.lifecycle+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epochStart).count();
    }
    return total;
}

#ifdef LAYOUT_REVIEW
[[maybe_unused]] static void IsolatedCosts(const std::vector<Recipe> &recipes) {
    using Old=previous_cache::PipelineLayoutCache;
    std::mutex mutex;
    std::vector<Old::DescriptorKey> keys;keys.reserve(recipes.size());
    for(int round=0;round<11;++round)for(int stage=0;stage<4;++stage) {
        int64_t total{};
        for(int epoch=0;epoch<3000;++epoch) {
            auto start=std::chrono::steady_clock::now();
            for(const auto &r:recipes) {
                if(stage==0) {
                    auto hash=Old::HashBindings(r.flags,r.bindings);
                    asm volatile("" : : "g"(hash) : "memory");
                } else if(stage==1) {
                    auto hash=Old::PipelineHash::Hash(17,r.pipelineFlags,r.ranges);
                    asm volatile("" : : "g"(hash) : "memory");
                } else if(stage==2) {
                    Old::DescriptorKey key{r.flags,{},17};
                    key.bindings.reserve(r.bindings.size());
                    for(const auto &b:r.bindings)key.bindings.push_back({b.binding,b.descriptorType,b.descriptorCount,b.stageFlags});
                    keys.push_back(std::move(key));
                } else { std::scoped_lock lock(mutex);asm volatile("" : : : "memory"); }
            }
            total+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            keys.clear(); // Key destruction is outside its construction measurement.
        }
        std::cout<<"{\"kind\":\"isolated\",\"round\":"<<round<<",\"stage\":"<<stage
            <<",\"epochs\":3000,\"requests\":"<<recipes.size()<<",\"total_ns\":"<<total<<"}\n";
    }
}
#endif

int main(int argc,char **argv) {
    try {
        if(argc!=2)throw std::runtime_error("usage: benchmark fixture.txt");
        auto recipes=Load(argv[1]);
#ifdef LAYOUT_DIAGNOSTICS
        test::Context measured(false,true);
        test::Check(measured.device.getDispatcher()->vkCreateDescriptorSetLayout==test::CreateSet,"object-count device is not instrumented");
        for(int round=0;round<5;++round) {
            test::counts.Reset();Run<PipelineLayoutCache>(measured.device,recipes,"master",false,1,true);test::counts.Reset();
#ifdef LAYOUT_REVIEW
            Run<previous_cache::PipelineLayoutCache>(measured.device,recipes,"previous",true,1,true);test::counts.Reset();
#endif
            Run<PipelineLayoutCache>(measured.device,recipes,"revised",true,1,true);test::counts.Reset();
        }
#else
        test::Context plain(false,false);
        test::Check(plain.device.getDispatcher()->vkCreateDescriptorSetLayout!=test::CreateSet,"timing device is instrumented");
        auto props=plain.physical.getProperties();
        std::cout<<"{\"kind\":\"device\",\"name\":\""<<props.deviceName.data()<<"\",\"driver_version\":"<<props.driverVersion<<",\"cpu\":"<<sched_getcpu()<<"}\n";
        auto run=[&](int variant,int epochs) {
            switch(variant) {
                case 0:return Run<PipelineLayoutCache>(plain.device,recipes,"master",false,epochs,false);
                case 1:return Run<PipelineLayoutCache>(plain.device,recipes,"revised",true,epochs,false);
#ifdef LAYOUT_REVIEW
                case 2:return Run<previous_cache::PipelineLayoutCache>(plain.device,recipes,"previous",true,epochs,false);
                case 3:return Run<unlocked_cache::PipelineLayoutCache>(plain.device,recipes,"previous_no_lock",true,epochs,false);
#endif
                default:throw std::runtime_error("bad variant");
            }
        };
#ifdef LAYOUT_REVIEW
        constexpr int variants=4;
#else
        constexpr int variants=2;
#endif
        const char *names[]={"master","revised","previous","previous_no_lock"};
        for(int variant=0;variant<variants;++variant)run(variant,25);
        for(int round=0;round<12;++round)for(int pass=0;pass<variants;++pass) {
            int variant=(round+pass)%variants;
            auto ns=run(variant,300);
            std::cout<<"{\"kind\":\"timing\",\"round\":"<<round<<",\"variant\":\""<<names[variant]
                <<"\",\"epochs\":300,\"requests_per_epoch\":"<<recipes.size()<<",\"total_creation_ns\":"<<ns.creation
                <<",\"total_lifecycle_ns\":"<<ns.lifecycle<<"}\n";
        }
#ifdef LAYOUT_REVIEW
        IsolatedCosts(recipes);
#endif
#endif
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
