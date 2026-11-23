#include "benchmark_inputs.h"
#include "previous-layout-cache.h"
#include "profile-layout-cache.h"
#include <chrono>

// Both headers are generated from exact source, with visibility opened solely for
// diagnostics. Precreate real Vulkan Entry objects outside insertion/lookup timing.
using Old = previous_cache::PipelineLayoutCache;
using Current = profile_cache::PipelineLayoutCache;
using Entry = Old::Entry;
using Owner = std::shared_ptr<Entry>;
using PrimeSet = decltype(std::declval<Old>().entries);
using CurrentOwner = std::shared_ptr<Current::Entry>;
using PowerSet = decltype(std::declval<Current>().entries);
struct RawHash { size_t operator()(const Entry *e) const { return e->key.hash; } };
struct RawEqual {
    bool operator()(const Entry *a,const Entry *b) const { return Old::DescriptorEqual{}(a->key,b->key); }
};
struct Counts { inline static size_t hashes{}, comparisons{}; };
template<class Cache> struct CountHash {
    using Owner=std::shared_ptr<typename Cache::Entry>;
    size_t operator()(const Owner &e) const { ++Counts::hashes;return typename Cache::EntryHash{}(e); }
    size_t operator()(const typename Cache::DescriptorProbe &p) const { ++Counts::hashes;return typename Cache::EntryHash{}(p); }
};
template<class Cache> struct CountEqual {
    using Owner=std::shared_ptr<typename Cache::Entry>;
    using is_transparent=void;
    bool operator()(const Owner &a,const Owner &b) const { ++Counts::comparisons;return typename Cache::EntryEqual{}(a,b); }
    bool operator()(const Owner &a,const typename Cache::DescriptorProbe &b) const { ++Counts::comparisons;return typename Cache::EntryEqual{}(a,b); }
};
template<class Cache,class Set> void CountReplay(const std::vector<std::shared_ptr<typename Cache::Entry>> &entries,
                                   const std::vector<typename Cache::DescriptorProbe> &probes,const char *name) {
    Counts::hashes=0;Counts::comparisons=0;
    Set set;
    for(size_t i=0;i<entries.size();++i){set.find(probes[i]);set.insert(entries[i]);}
    std::cout<<"{\"kind\":\"profile_counts\",\"variant\":\""<<name<<"\",\"hash_calls\":"<<Counts::hashes
        <<",\"comparison_calls\":"<<Counts::comparisons<<",\"unique_interfaces\":"<<set.size()<<"}\n";
}
int main(int argc,char **argv) {
    try {
        if(argc!=2)throw std::runtime_error("usage: miss_profile unique-fixture.txt");
        auto recipes=Load(argv[1]);test::Context v(false,false);
        std::vector<Old::DescriptorProbe> probes,missProbes;
        std::vector<Current::DescriptorProbe> currentProbes;
        std::vector<Owner> entries;
        std::vector<CurrentOwner> currentEntries;
        std::vector<Old::DescriptorKey> keys;
        PrimeSet populated;
        for(const auto &r:recipes) {
            probes.push_back({r.flags,r.bindings,Old::HashBindings(r.flags,r.bindings)});
            auto p=probes.back();currentProbes.push_back({p.flags,p.bindings,p.hash});
            auto changed=r.flags^vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
            missProbes.push_back({changed,r.bindings,Old::HashBindings(changed,r.bindings)});
            keys.emplace_back(p);
            entries.push_back(std::make_shared<Entry>(v.device,p,r.Info(),r.ranges,r.pipelineFlags));
            currentEntries.push_back(std::make_shared<Current::Entry>(v.device,currentProbes.back(),r.Info(),r.ranges,r.pipelineFlags));
            if(!populated.insert(entries.back()).second)
                throw std::runtime_error("miss profiling requires unique descriptor interfaces");
        }
        // libstdc++ uses a 16-byte control block here; report this as a sized raw
        // allocation control, not an exact timing of make_shared construction.
        const size_t allocationSize=sizeof(Entry)+2*sizeof(void *);
        std::vector<void *> allocations;allocations.reserve(recipes.size());
        std::vector<Old::DescriptorKey> made;made.reserve(recipes.size());
        std::vector<Owner> copied;copied.reserve(recipes.size());
        std::vector<Owner> constructed;constructed.reserve(recipes.size());
        std::vector<std::shared_ptr<Old::Layouts>> layouts;layouts.reserve(recipes.size());
        std::mutex mutex;
        constexpr const char *names[]={"descriptor_hash","key_construction","compare_equal","compare_different",
            "sized_raw_allocations","prime_insert_copy","power_insert_copy","prime_insert_raw",
            "uncontended_lock","shared_owner_copy","prime_lookup_hit","shared_native_pair",
            "shared_entry_native_pair","prime_lookup_miss","empty_loop","hash_finalizer",
            "prime_miss_index_replay","power_miss_index_replay_move"};
        for(int round=0;round<12;++round)for(int step=0;step<18;++step) {
            int64_t total{};
            for(int epoch=0;epoch<1000;++epoch) {
                PrimeSet fresh;PowerSet power;
                tsl::robin_pg_set<Entry *,RawHash,RawEqual,std::allocator<Entry *>,true> raw;
                auto movable=currentEntries; // Copy setup excluded from move-insertion control.
                auto start=std::chrono::steady_clock::now();
                for(size_t i=0;i<recipes.size();++i) {
                    const auto &r=recipes[i];const auto &p=probes[i];
                    switch(step) {
                        case 0: {auto h=Old::HashBindings(r.flags,r.bindings);asm volatile(""::"g"(h):"memory");break;}
                        case 1: made.emplace_back(p);break;
                        case 2: {bool equal=Old::DescriptorEqual{}(keys[i],p);asm volatile(""::"g"(equal):"memory");break;}
                        case 3: {bool equal=Old::DescriptorEqual{}(keys[i],probes[(i+1)%recipes.size()]);asm volatile(""::"g"(equal):"memory");break;}
                        case 4: allocations.push_back(::operator new(allocationSize));break;
                        case 5: fresh.insert(entries[i]);break;
                        case 6: power.insert(currentEntries[i]);break;
                        case 7: raw.insert(entries[i].get());break;
                        case 8: {std::scoped_lock lock(mutex);asm volatile("":::"memory");break;}
                        case 9: copied.push_back(entries[i]);break;
                        case 10: {auto it=populated.find(p);asm volatile(""::"g"(it!=populated.end()):"memory");break;}
                        case 11: layouts.push_back(std::make_shared<Old::Layouts>(v.device,r.Info(),r.ranges,r.pipelineFlags,nullptr));break;
                        case 12: constructed.push_back(std::make_shared<Entry>(v.device,p,r.Info(),r.ranges,r.pipelineFlags));break;
                        case 13: {auto it=populated.find(missProbes[i]);asm volatile(""::"g"(it!=populated.end()):"memory");break;}
                        case 14: asm volatile("":::"memory");break;
                        case 15: {auto h=Current::FinalizeHash(p.hash);asm volatile(""::"g"(h):"memory");break;}
                        case 16: {auto found=fresh.find(p)!=fresh.end();asm volatile(""::"g"(found):"memory");fresh.insert(entries[i]);break;}
                        case 17: {auto found=power.find(currentProbes[i])!=power.end();asm volatile(""::"g"(found):"memory");power.insert(std::move(movable[i]));break;}
                    }
                }
                total+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
                made.clear();for(auto *a:allocations)::operator delete(a);allocations.clear();
                copied.clear();layouts.clear();constructed.clear(); // Destruction excluded from stage timing.
            }
            std::cout<<"{\"kind\":\"miss_profile\",\"round\":"<<round<<",\"stage\":\""<<names[step]
                <<"\",\"epochs\":1000,\"requests\":"<<recipes.size()<<",\"total_ns\":"<<total<<"}\n";
        }
        std::cout<<"{\"kind\":\"profile_sizes\",\"entry_bytes\":"<<sizeof(Entry)<<",\"allocation_control_bytes\":"<<allocationSize<<"}\n";
        using CountPrime=tsl::robin_pg_set<Owner,CountHash<Old>,CountEqual<Old>,std::allocator<Owner>,true>;
        using CountPower=tsl::robin_set<CurrentOwner,CountHash<Current>,CountEqual<Current>,std::allocator<CurrentOwner>,true>;
        CountReplay<Old,CountPrime>(entries,probes,"c0a47a2c");
        CountReplay<Current,CountPower>(currentEntries,currentProbes,"revised");
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
