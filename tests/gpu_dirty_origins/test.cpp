#include "ssbo_diagnostics.h"
#include <source_location>
#include <vector>
#include <span>
#include <optional>
#include <mutex>
#include <cstring>
#include <cassert>
#include <iostream>
#include <memory>
#include <thread>
namespace ssbo_diagnostics=skyline::gpu::ssbo_diagnostics;
namespace util {uint64_t GetTimeNs(){static uint64_t t;return ++t;}}
#define TRACE_EVENT(...)
struct Buffer {
 enum class DirtyState{Clean,CpuDirty,GpuDirty};DirtyState dirtyState{DirtyState::CpuDirty};
 unsigned id{};bool guest=true,isDirect=false;std::recursive_mutex stateMutex;
 struct Trap{void TrapRegions(int,bool){}};struct Process{Trap trap;};Process process;
 struct GPU {struct State{Process *process;}state;};GPU gpu{{&process}};
 std::optional<int> trapHandle{1};std::vector<unsigned char> source=std::vector<unsigned char>(8470528,0x37);
 std::span<unsigned char> mirror{source};std::optional<std::vector<unsigned char>> backing{std::vector<unsigned char>(8470528,0x37)};
 unsigned waits{},advances{};void WaitOnFence(){++waits;}void AdvanceSequence(){++advances;}
 void SynchronizeHost(bool=false,std::source_location=std::source_location::current());
};
#include "host.inc"
int main(){
 Buffer b;b.source[512]=0x19;b.SynchronizeHost();
 assert(b.dirtyState==Buffer::DirtyState::Clean && *b.backing==b.source && b.waits==1 && b.advances==1);
 uint64_t calls{},bytes{};for(auto &e:ssbo_diagnostics::dirty.entries)if(e.state==2){calls+=e.calls;bytes+=e.copied;assert(std::strstr(e.key.caller,"main"));}
 assert(calls==1 && bytes==8470528);
 b.SynchronizeHost();calls=bytes=0;for(auto &e:ssbo_diagnostics::dirty.entries)if(e.state==2){calls+=e.calls;bytes+=e.copied;}
 assert(calls==2 && bytes==8470528 && b.waits==1);
 auto key=ssbo_diagnostics::DirtyKey{0,512,1,ssbo_diagnostics::Origin::CpuWrite,1,1,1,"test",1};
 ssbo_diagnostics::dirty.Record(key);ssbo_diagnostics::dirty.Record(key);
 uint64_t regions{};for(auto &e:ssbo_diagnostics::dirty.entries)if(e.state==2 && e.key.extent==1){++regions;assert(e.calls==2 && e.key.size==1);}
 assert(regions==1);
 auto t=std::make_unique<ssbo_diagnostics::DirtyTable>();
 for(unsigned i=0;i<33;++i)t->Record({172,uint64_t(i)*4096,4,ssbo_diagnostics::Origin::CpuWrite,1,1,1,"test",1});
 assert(t->dropped==0);
 std::vector<std::thread> workers;
 for(unsigned n=0;n<4;++n)workers.emplace_back([&]{for(unsigned i=0;i<1000;++i)t->Record({555,128,8,ssbo_diagnostics::Origin::CpuWrite,1,1,1,"thread",2});});
 for(auto &w:workers)w.join();
 unsigned matches{};
 for(auto &e:t->entries) {if(e.state==2 && e.key.buffer==555){assert(e.calls==4000);++matches;}}
 assert(matches==1);
 std::cout<<"PASS exact SynchronizeHost: whole-copy reproduction, no-op, caller provenance, known-region aggregation\n";
}
