// SPDX-License-Identifier: MPL-2.0
#include <common.h>
#include <common/file_descriptor.h>
#define private public
#include <kernel/memory.h>
#undef private
#include <services/jit/IJitService.h>
#include <kernel/types/KProcess.h>
#include <kernel/types/KTransferMemory.h>
#include <kernel/results.h>
#include <mbedtls/sha256.h>
#include <cassert>
#include <iostream>
#include "fixture.h"
using namespace skyline;
using namespace skyline::service::jit;
int main() {
    DeviceState state; state.process=std::make_shared<kernel::type::KProcess>(state);
    auto &memory=state.process->memory;
    auto *base=static_cast<u8*>(mmap(nullptr,0x2000000,PROT_NONE,MAP_SHARED|MAP_ANONYMOUS,-1,0)); assert(base!=MAP_FAILED);
    memory.base={base,0x2000000}; memory.addressSpace=memory.base;
    memory.addressSpaceType=memory::AddressSpaceType::AddressSpace39Bit;
    memory.chunks={{base,{.state=memory::states::Unmapped,.size=memory.base.size()}},{base+memory.base.size(),{.state=memory::states::Reserved}}};
    memory.code=span<u8>{base,0x200000}; memory.alias=span<u8>{base+0x800000,0x200000}; memory.heap=span<u8>{base+0x1000000,0x800000};
    memory.MapCodeMemory(memory.code.guest,memory::Permission{true,false,true});
    auto rxSource=span<u8>{memory.heap.guest.data(),0x2000};
    auto roSource=span<u8>{rxSource.data()+0x2000,0x2000};
    auto tmSource=span<u8>{rxSource.data()+0x4000,0x1000};
    memory.MapHeapMemory(rxSource);memory.MapHeapMemory(roSource);memory.MapHeapMemory(tmSource);
    auto rx=std::make_shared<kernel::type::KCodeMemory>(state,rxSource);
    auto ro=std::make_shared<kernel::type::KCodeMemory>(state,roSource);
    auto tm=std::make_shared<kernel::type::KTransferMemory>(state,tmSource);
    assert(rx->Initialize()==Result{});assert(ro->Initialize()==Result{});
    state.process->handles={{1,state.process},{2,rx},{3,ro},{4,tm}};
    service::ServiceManager manager; IJitService service(state,manager);
    kernel::type::KSession session; kernel::ipc::IpcRequest create; kernel::ipc::IpcResponse created;
    create.SetArguments(std::array<u64,2>{0x2000,0x2000});create.copyHandles={1,2,3};
    assert(service.CreateJitEnvironment(session,create,created)==Result{});
    auto environment=std::static_pointer_cast<IJitEnvironment>(manager.registered);
    kernel::ipc::IpcRequest empty;kernel::ipc::IpcResponse addresses;
    assert(environment->GetCodeAddress(session,empty,addresses)==Result{});
    u64 rxAddress{},roAddress{};std::memcpy(&rxAddress,addresses.payload.data(),8);std::memcpy(&roAddress,addresses.payload.data()+8,8);
    assert(rxAddress && roAddress && rxAddress!=roAddress);
    auto nro=Fixture();
    const u32 code[]={0x52800020,0xd65f03c0,0xb900001f,0xd65f03c0,0xd65f03c0,0xb900001f,0xd2800000,0xd65f03c0,
                     0xb900001f,0xf9400068,0x5280a809,0x72aa5009,0xb9000109,0x52807809,0x72bacbe9,0xb9000509,
                     0xd2800109,0xa9002428,0xa9007c5f,0xf9400fea,0x528009ab,0xb900014b,0xd65f03c0};
    std::memcpy(nro.data()+0x200,code,sizeof(code));
    // Error-return fixture: ret contains the command's low word, wrapper returns zero.
    const u32 controlCode[]={0xb9000002,0xd2800000,0xd65f03c0};
    std::memcpy(nro.data()+0x300,controlCode,sizeof(controlCode));
    Put(nro,0x220,u32{0xb9000004}); // GenerateCode ret likewise receives command (w4).
    const std::pair<const char*,u64> names[]={{"nnjitpluginGetVersion",0x200},{"nnjitpluginConfigure",0x208},
       {"nnjitpluginOnPrepared",0x210},{"nnjitpluginControl",0x300},{"nnjitpluginGenerateCode",0x220}};
    size_t stringAt=1,symbolAt=1;
    for(auto [name,address]:names) {std::strcpy(reinterpret_cast<char*>(nro.data()+0x1600+stringAt),name);
       Put(nro,0x1400+symbolAt++*24,Elf64_Sym{static_cast<u32>(stringAt),ELF64_ST_INFO(STB_GLOBAL,STT_FUNC),0,1,address,4});stringAt+=std::strlen(name)+1;}
    Put(nro,0x1704,u32{6});Put(nro,0x1120,Elf64_Dyn{DT_STRSZ,{0x100}});
    std::vector<u8> nrr(0x1000);Put(nrr,0,u32{0x3052524e});Put(nrr,0x338,u32{0x1000});nrr[0x33c]=1;
    Put(nrr,0x340,u32{0x350});Put(nrr,0x344,u32{1});
    assert(mbedtls_sha256_ret(nro.data(),nro.size(),nrr.data()+0x350,0)==0);
    kernel::ipc::IpcRequest load;kernel::ipc::IpcResponse loaded;
    load.SetArguments(u64{0x1000});load.copyHandles={4};load.inputBuf={span<u8>{nrr},span<u8>{nro}};
    nrr[0x350]^=1; assert(environment->LoadPlugin(session,load,loaded)!=Result{});
    nrr[0x350]^=1;load.position=0; assert(environment->LoadPlugin(session,load,loaded)==Result{});
    std::array<u8,4> input{},output{};
    kernel::ipc::IpcRequest control; kernel::ipc::IpcResponse controlled;
    control.SetArguments(u64{});control.inputBuf={span<u8>{input}};control.outputBuf={span<u8>{output}};
    assert(environment->Control(session,control,controlled)==Result{});
    assert(controlled.payload.size()==4);
    kernel::ipc::IpcRequest emptyControl;kernel::ipc::IpcResponse emptyControlled;
    emptyControl.SetArguments(u64{});
    assert(environment->Control(session,emptyControl,emptyControlled)==Result{});
    kernel::ipc::IpcRequest failedControl;kernel::ipc::IpcResponse failedControlled;
    failedControl.SetArguments(u64{0x2ee202});
    assert(environment->Control(session,failedControl,failedControlled)==kernel::result::InvalidState);
    u32 callbackResult{};std::memcpy(&callbackResult,failedControlled.payload.data(),4);
    assert(callbackResult==0x2ee202);
    assert(std::any_of(test::logs.begin(),test::logs.end(),[](const auto &log) {
        return log.find("JIT Control callback error:")!=std::string::npos &&
               log.find("plugin_result=0x002EE202")!=std::string::npos;
    }));
    struct Range {u64 offset,size;};
    struct Arguments {u32 dataSize,padding;u64 command;Range in0,in1;std::array<u64,4> data;};
    kernel::ipc::IpcRequest generate;kernel::ipc::IpcResponse generated;
    generate.SetArguments(Arguments{32,0,0,{rxAddress,0x2000},{roAddress,0x2000},{}});
    generate.inputBuf={span<u8>{input}};generate.outputBuf={span<u8>{output}};
    assert(environment->GenerateCode(session,generate,generated)==Result{});
    assert(generated.payload.size()==0x28);
    Range range;std::memcpy(&range,generated.payload.data()+8,sizeof(range));
    assert(range.offset==rxAddress && range.size==8);
    const u32 expected[]={0x52800540,0xd65f03c0};
    assert(std::memcmp(reinterpret_cast<void*>(rxAddress),expected,sizeof(expected))==0);
    u32 out;std::memcpy(&out,output.data(),4);assert(out==77);
    // Execute the ARM64 emitted by the NRO, using a CPU with an executable view.
    PluginImage image;image.Load(nro,0x10000,[](std::string_view){return u64{};});
    PluginContext executor;executor.Load(image);auto backing=rx->GetWritableBacking();
    executor.Map(rxAddress,{backing.data(),backing.size()},false,true);
    assert(executor.Call(rxAddress,{})==42);
    kernel::ipc::IpcRequest failedGenerate;kernel::ipc::IpcResponse failedGenerated;
    failedGenerate.SetArguments(Arguments{32,0,0x2ee202,{rxAddress,0x2000},{roAddress,0x2000},{}});
    failedGenerate.inputBuf={span<u8>{input}};failedGenerate.outputBuf={span<u8>{output}};
    assert(environment->GenerateCode(session,failedGenerate,failedGenerated)==kernel::result::InvalidState);
    std::memcpy(&callbackResult,failedGenerated.payload.data(),4);assert(callbackResult==0x2ee202);
    assert(std::any_of(test::logs.begin(),test::logs.end(),[](const auto &log) {
        return log.find("JIT GenerateCode callback error:")!=std::string::npos &&
               log.find("plugin_result=0x002EE202")!=std::string::npos;
    }));
    manager.registered.reset();environment.reset();state.process->handles.clear();
    std::cout<<"jit:u IPC: CodeMemory, NRR hash rejection, NRO load, callbacks, ARM64 generation and execution passed\n";
}
