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
#include "../../app/src/main/cpp/skyline/common/fault_access.h"
#include "trap-scope.inc"
namespace ssbo_diagnostics=skyline::gpu::ssbo_diagnostics;
namespace util {uint64_t GetTimeNs(){static uint64_t t;return ++t;}}
#define TRACE_EVENT(...)
struct Buffer {
 enum class DirtyState{Clean,CpuDirty,GpuDirty};DirtyState dirtyState{DirtyState::CpuDirty};
 uint64_t diagnosticEpoch{1};unsigned diagnosticCause{};
 bool DiagnosticSample() const;unsigned DiagnosticFlags();
 void DiagnosticCycle(unsigned,unsigned,unsigned,uint64_t=0,uint64_t=0);
 std::shared_ptr<int> cycle;bool AllCpuBackingWritesBlocked(){return dirtyState==DirtyState::GpuDirty;}
 unsigned id{};bool guest=true,isDirect=false;std::recursive_mutex stateMutex;
 struct Trap{void TrapRegions(int,bool){}};struct Process{Trap trap;};Process process;
 struct Settings{std::optional<bool> enableFastGpuReadbackHack{true},enableFastReadbackWrites{true};}settings;
 struct GPU {struct State{Process *process;Settings *settings;}state;};GPU gpu{{&process,&settings}};
 std::optional<int> trapHandle{1};std::vector<unsigned char> source=std::vector<unsigned char>(8470528,0x37);
 std::span<unsigned char> mirror{source};std::optional<std::vector<unsigned char>> backing{std::vector<unsigned char>(8470528,0x37)};
 unsigned waits{},advances{};void WaitOnFence(){++waits;}void AdvanceSequence(){++advances;}
 void SynchronizeHost(bool=false,std::source_location=std::source_location::current());
};
#include "host.inc"
#include "fast.inc"
int main(){
 ssbo_diagnostics::buffers.Init(0,8470528,0,false);ssbo_diagnostics::buffers.Bind(0);
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
 // Sampled upload with identical bytes still performs the real copy.
 b.diagnosticEpoch=129;b.dirtyState=Buffer::DirtyState::CpuDirty;b.SynchronizeHost();
 bool sawDifferent{},sawEqual{};
 for(auto &e:ssbo_diagnostics::cycles.entries)if(e.ready && e.event.phase==4){sawDifferent|=e.event.equal==0;sawEqual|=e.event.equal==1;assert(e.event.bytes==8470528);}
 assert(sawDifferent && sawEqual);
 // Exact fast callback executes BEFORE the guest store. Preserve its existing
 // option-dependent behavior; this experiment does not change that policy.
 for(bool writes:{false,true}) {
  Buffer f;f.diagnosticEpoch=128;f.settings.enableFastReadbackWrites=writes;
  f.dirtyState=Buffer::DirtyState::GpuDirty;(*f.backing)[16]=0x59;
  assert(FastCallback(&f));assert(f.source[16]==0x59);
  assert(f.dirtyState==(writes?Buffer::DirtyState::CpuDirty:Buffer::DirtyState::Clean));
  f.source[16]=0x77; // Actual guest store resumes after the callback.
  f.SynchronizeHost();assert((*f.backing)[16]==(writes?0x77:0x59));
 }
 auto order=std::make_unique<ssbo_diagnostics::CycleTable>();
 std::vector<std::thread> producers;
 for(unsigned n=0;n<4;++n)producers.emplace_back([&]{for(unsigned i=0;i<100;++i)order->Record({.buffer=1,.epoch=i});});
 unsigned observed{};
 while(observed<400) {
  const auto &slot{order->entries[observed]};
  if(slot.ready.load(std::memory_order_acquire)){assert(slot.event.buffer==1 && slot.event.epoch<100);++observed;}
  else std::this_thread::yield();
 }
 for(auto &w:producers){w.join();}
 assert(order->next==400);
 for(unsigned i=0;i<400;++i)assert(order->entries[i].ready && order->entries[i].event.buffer==1);
 for(unsigned i=400;i<2051;++i)order->Record({.buffer=2,.epoch=i});
 assert(order->dropped==3);
 {using namespace skyline::trap_diagnostics;
  assert(!faultAddress && !writeAccess);
  FaultScope outer{b.source.data()+512,true};assert(faultAddress==b.source.data()+512 && writeAccess);
  try{FaultScope inner{b.source.data()+1024,false};assert(!writeAccess);throw 1;}catch(int){}
  assert(faultAddress==b.source.data()+512 && writeAccess);
 }
 assert(!skyline::trap_diagnostics::faultAddress && !skyline::trap_diagnostics::writeAccess);
 {using namespace skyline::fault_diagnostics;
  std::array<std::byte,64> frame{};
  const auto put=[&](size_t offset,uint32_t magic,uint32_t size,uint64_t esr){
   std::memcpy(frame.data()+offset,&magic,4);std::memcpy(frame.data()+offset+4,&size,4);std::memcpy(frame.data()+offset+8,&esr,8);
  };
  assert(Access(frame)==2);
  put(0,0x45535201,16,0x90000000);assert(Access(frame)==0);
  put(0,0x45535201,16,0x90000040);assert(Access(frame)==1);
  put(0,0x45535201,16,0x94000040);assert(Access(frame)==1);
  put(0,0x45535201,16,0x90000140);assert(Access(frame)==2); // CM, not a data store.
  put(0,0x45535201,16,0x80000000);assert(Access(frame)==2); // Instruction abort.
  put(0,0x45535201,32,0x90000040);assert(Access(frame)==2);
  put(0,0x45535201,0,0x90000040);assert(Access(frame)==2);
  put(0,0x45535201,16,0x90000040);assert(Access(std::span<const std::byte>{frame}.first(15))==2);
  put(0,0x46508001,32,0);put(32,0x45535201,16,0x90000000);assert(Access(frame)==0);
 }
 {using namespace skyline::trap_diagnostics;
  SignalScope signal{0x1234,0};assert(faultPc==0x1234 && actualAccess==0);
  Buffer read;read.diagnosticEpoch=128;read.dirtyState=Buffer::DirtyState::GpuDirty;
  FaultScope routed{read.source.data()+16,true}; // NCE preserves true even for a known read.
  assert(FastCallback(&read));assert(read.dirtyState==Buffer::DirtyState::CpuDirty);
  read.SynchronizeHost(); // No guest store in this READ scenario; same bytes still uploaded.
 }
 assert(skyline::trap_diagnostics::faultPc==0 && skyline::trap_diagnostics::actualAccess==2);
 bool observedReadRoutedAsWrite{};
 for(auto &slot:ssbo_diagnostics::cycles.entries)if(slot.ready.load(std::memory_order_acquire)) {
  const auto &e=slot.event;
  observedReadRoutedAsWrite|=e.phase==2 && e.actualAccess==0 && e.faultPc==0x1234 && (e.flags&16);
 }
 assert(observedReadRoutedAsWrite);
 unsigned matches{};uint64_t recorded{};
 for(auto &e:t->entries) {if(e.state==2 && e.key.buffer==555){recorded+=e.calls;++matches;}}
 assert(matches==1 && recorded+t->dropped==4000);
 std::cout<<"PASS exact SynchronizeHost: whole-copy reproduction, no-op, caller provenance, known-region aggregation\n";
}
