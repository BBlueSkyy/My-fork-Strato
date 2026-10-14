// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <cstring>
#include <iostream>
#include <nce/svc_callsite.h>
#include <nce/guest.h>
#include <nce/trampoline.h>
static_assert(offsetof(skyline::nce::ThreadContext,hostTpidrEl0)==0x2a0);
static_assert(offsetof(skyline::nce::ThreadContext,hostSp)==0x2a8);
static_assert(offsetof(skyline::nce::ThreadContext,nzcv)==0x2c0);
static_assert(offsetof(skyline::nce::ThreadContext,svcCallsite)==0x2d8);
int main() {
    using namespace skyline::nce;
    std::array<skyline::u32,TrampolineSize+1> trampoline{};
    trampoline.back()=0xfeedface;
    assert(WriteTrampoline(trampoline.data(),0x123456789ABCDEF0)==trampoline.data()+TrampolineSize);
    assert(trampoline.back()==0xfeedface);
    // Target goes to X5; X4 must preserve the per-SVC immediate PC argument.
    for(size_t i=8;i<12;i++) assert((trampoline[i]&31)==registers::X5);
    assert(trampoline[12]==0xf94007e2 && trampoline[13]==0xaa1d03e3 && trampoline[14]==0xd63f00a0);
    std::array<std::uint64_t,6> memory{0x1010,0x8004,0x1020,0x8010,0,0x8020};
    auto read=[&](std::uint64_t address,void *out,size_t size) {
        if(address<0x1000 || address-0x1000>sizeof(memory) || size>sizeof(memory)-(address-0x1000)) return false;
        std::memcpy(out,reinterpret_cast<const char*>(memory.data())+(address-0x1000),size);return true;
    };
    assert((WalkGuestFrames(0x1000,read)==std::vector<std::uint64_t>{0x8004,0x8010,0x8020}));
    memory[2]=0x1000; // cycle: stop after the current frame
    assert(WalkGuestFrames(0x1000,read).size()==2);
    assert(WalkGuestFrames(0x2000,read).empty());
    assert(WalkGuestFrames(0x1001,read).empty());
    std::cout<<"Native SVC callsite and bounded guest-frame reads passed\n";
}
