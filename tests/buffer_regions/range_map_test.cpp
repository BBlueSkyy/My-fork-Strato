// SPDX-License-Identifier: MPL-2.0
#include "host_types.h"
#include "interval_map.h"
#include <cstdlib>
#include <new>
#include <cstdio>
#include <sys/mman.h>
#include "trap_manager.h"
static int lastPermission{};
static int Protect(void* p,size_t size,int permission){lastPermission=permission;return ::mprotect(p,size,permission);}
#define mprotect Protect
#define TRACE_EVENT(...)
#include "trap.inc"
#undef mprotect
static unsigned failOn{};
void* operator new(std::size_t size) {if(failOn&&!--failOn)throw std::bad_alloc();if(auto p=std::malloc(size))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
struct Value {
    inline static int live{};
    int id;
    explicit Value(int id):id{id}{++live;}
    Value(const Value& other):id{other.id}{++live;}
    Value(Value&& other) noexcept:id{other.id}{++live;}
    ~Value(){--live;}
};
int main(){
    unsigned injected{};
    for(unsigned mode{};mode<3;++mode)for(unsigned allocation=1;allocation<14;++allocation){
        skyline::IntervalMap<uint8_t*,Value> map;
        uint8_t bytes[128]{};Value stable{1},candidate{2};map.Insert(bytes,bytes+16,stable);
        int before=Value::live;bool failed{};failOn=allocation;
        try {
            if(mode==0)map.Insert(bytes+32,bytes+64,candidate);
            if(mode==1){decltype(map)::Interval regions[]{{bytes+32,bytes+48},{bytes+64,bytes+80}};map.Insert(skyline::span<decltype(map)::Interval>{regions,2},candidate);}
            if(mode==2){skyline::span<uint8_t> regions[]{{bytes+32,16},{bytes+64,16}};map.Insert(skyline::span<skyline::span<uint8_t>>{regions,2},candidate);}
        }catch(const std::bad_alloc&){failed=true;++injected;}
        failOn=0;
        if(failed && (Value::live!=before||map.Get(bytes+40)||map.Get(bytes+70))){std::fputs("FAIL failed interval registration retained payload or partial callbacks\n",stderr);return 1;}
        if(!map.Get(bytes+4)||map.Get(bytes+4)->id!=1){std::fputs("FAIL existing trap mapping changed\n",stderr);return 1;}
    }
    {
        auto page=static_cast<uint8_t*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(page==MAP_FAILED)return 1;
        skyline::TrapManager traps;
        skyline::span<uint8_t> all{page,8192},second{page+4096,4096};
        auto a=traps.CreateTrap(all,[]{},[]{return true;},[]{return true;});
        auto b=traps.CreateTrap(second,[]{},[]{return true;},[]{return true;});
        traps.TrapRegions(a,true);traps.TrapRegions(b,false);
        failOn=1;traps.TrapRegions(a,true);failOn=0;
        if(lastPermission!=PROT_NONE)return 1;
        failOn=1;bool failed{};
        try{traps.RemoveTrap(a);}catch(const std::bad_alloc&){failed=true;}
        failOn=0;
        if(failed||lastPermission!=PROT_NONE){std::fputs("FAIL trap reprotection allocated or weakened overlapping GPU protection\n",stderr);return 1;}
        failOn=1;traps.DeleteTrap(a);traps.DeleteTrap(b);failOn=0;
        if(lastPermission!=(PROT_READ|PROT_WRITE|PROT_EXEC))return 1;
        munmap(page,8192);
        std::puts("PASS allocation-free trap protection/deletion preserves strongest overlap protection");
    }
    if(Value::live){std::fputs("FAIL leaked interval payload\n",stderr);return 1;}
    std::printf("PASS interval registration rollback across all overloads (%u injected allocation failures)\n",injected);
}
