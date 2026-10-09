// SPDX-License-Identifier: MPL-2.0
#include <common.h>
#include <common/file_descriptor.h>
// Set a compact VMM fixture instead of reserving Android's 140 GiB on Linux.
#define private public
#include <kernel/memory.h>
#undef private
#include <kernel/types/KCodeMemory.h>
#include <kernel/types/KProcess.h>
#include <kernel/results.h>
#include <cassert>
#include <cstring>
#include <iostream>
using namespace skyline;
template<class Manager> void CheckReadableMemory(Manager &manager, u64 owner, u64 writer, span<u8> source) {
    if constexpr (requires { manager.ReadMemoryIfReadable(owner, static_cast<void *>(nullptr), size_t{4}); }) {
        u32 value{};
        assert(manager.ReadMemoryIfReadable(owner,&value,sizeof(value)) && value==0x55555555);
        assert(!manager.ReadMemoryIfReadable(reinterpret_cast<u64>(source.data()),&value,sizeof(value)));
        assert(!manager.ReadMemoryIfReadable(~u64{},&value,sizeof(value)));
        assert(manager.ReadMemoryIfReadable(owner+source.size()-2,&value,sizeof(value)) && value==0x55555555);
        assert(!manager.ReadMemoryIfReadable(writer+source.size()-2,&value,sizeof(value)));
    } else assert(false && "permission-aware locked guest reads are missing");
}
int main() {
    DeviceState state; state.process=std::make_shared<kernel::type::KProcess>(state);
    auto &manager=state.process->memory;
    auto *base=static_cast<u8*>(mmap(nullptr,0x2000000,PROT_NONE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
    assert(base!=MAP_FAILED);
    manager.base={base,0x2000000}; manager.addressSpace=manager.base;
    manager.addressSpaceType=memory::AddressSpaceType::AddressSpace39Bit;
    manager.chunks={{base,{.state=memory::states::Unmapped,.size=manager.base.size()}},
                    {base+manager.base.size(),{.state=memory::states::Reserved}}};
    manager.code=span<u8>{base,0x200000};
    manager.alias=span<u8>{base+0x800000,0x200000};
    manager.heap=span<u8>{base+0x1000000,0x800000};
    manager.MapCodeMemory(manager.code.guest,memory::Permission{true,false,true});
    auto source=span<u8>{manager.heap.guest.data(),0x2000};
    manager.MapHeapMemory(source);
    auto memory=std::make_shared<kernel::type::KCodeMemory>(state,source);
    assert(memory->Initialize()==Result{});
    u64 owner{}; assert(memory->MapToOwner(source.size(),memory::Permission{true,false,true},owner)==Result{});
    u64 writer{}; assert(memory->MapAnywhere(source.size(),memory::Permission{true,true,false},writer)==Result{});
    assert(owner!=writer);
    std::memset(memory->GetWritableBacking().data(),0x55,source.size());
    memory->Synchronize(0,source.size());
    assert(*reinterpret_cast<u8*>(owner)==0x55 && *reinterpret_cast<u8*>(writer)==0x55);
    CheckReadableMemory(manager,owner,writer,source);
    assert(memory->MapToOwner(source.size(),memory::Permission{true,false,true},owner)==kernel::result::InvalidState);
    assert(memory->Unmap(writer,source.size()-0x1000,false)==kernel::result::InvalidSize);
    assert(memory->Unmap(writer,source.size(),false)==Result{});
    u32 snapshot{};assert(!manager.ReadMemoryIfReadable(writer,&snapshot,sizeof(snapshot)));
    assert(*reinterpret_cast<u8*>(owner)==0x55); // Unmap must never MADV_REMOVE shared pages.
    assert(memory->Unmap(owner,source.size(),true)==Result{});
    memory.reset(); assert(source[0]==0x55);
    assert(manager.ReadMemoryIfReadable(reinterpret_cast<u64>(source.data()),&snapshot,sizeof(snapshot)) && snapshot==0x55555555);
    std::cout<<"CodeMemory: real shared aliases, sizes, duplicate maps, unmap and source restore passed\n";
}
