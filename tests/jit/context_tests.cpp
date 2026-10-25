// SPDX-License-Identifier: MPL-2.0
#include <services/jit/plugin_context.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include "fixture.h"
using namespace skyline::service::jit;
int main() {
    // Actual ARM64 instructions: store the ninth argument, produce code, return.
    const std::uint32_t code[] = {0xf94003e8, 0xf9000008, 0x52800549, 0xb9000029, 0xd65f03c0};
    PluginContext context;
    auto bytes=Fixture(); std::memcpy(bytes.data()+0x200,code,sizeof(code));
    PluginImage image; image.Load(bytes,0x10000,[](std::string_view){return std::uint64_t{};});
    const std::uint32_t atomicCode[]={0x885f7c08,0x11000508,0x88097c08,0xd65f03c0};
    std::memcpy(bytes.data()+0x240,atomicCode,sizeof(atomicCode));
    image.Load(bytes,0x10000,[](std::string_view){return std::uint64_t{};});
    const std::uint32_t cacheCode[]={0xd50b7520,0xd5033fdf,0xd65f03c0};
    const std::uint32_t loopCode=0x14000000;
    std::memcpy(bytes.data()+0x280,&loopCode,4);
    std::memcpy(bytes.data()+0x290,cacheCode,sizeof(cacheCode));
    image.Load(bytes,0x10000,[](std::string_view){return std::uint64_t{};});
    context.Load(image);
    std::uint32_t output{};
    context.Map(0x80000000, {reinterpret_cast<std::uint8_t*>(&output),sizeof(output)}, true, false);
    auto result=context.Add(std::uint64_t{});
    context.Call(image.Symbol("nnjitpluginGetVersion"),{result,0x80000000,0,0,0,0,0,0,0xabcdef});
    assert(context.Get<std::uint64_t>(result)==0xabcdef); assert(output==42);
    // A private heap is reused per IPC call, not grown unboundedly.
    context.ResetHeap(); assert(context.Add(std::uint64_t{})==result);
    auto resolver=context.Helper("_resolve");
    auto name=context.Add("memset",7); auto helper=context.Call(resolver,{name});
    assert(helper==context.Helper("memset"));
    context.Call(0x10240,{0x80000000}); assert(output==43);
    auto writes=context.TakeWrites(0x80000000); assert(writes.first==0 && writes.second==sizeof(output));
    assert(context.TakeWrites(0x80000000).second==0);
    context.Call(helper,{0x80000000,0x12,sizeof(output)}); assert(output==0x12121212);
    auto denied=false;
    try { context.Call(helper,{0x80000002,0,4}); } catch(const std::exception &) { denied=true; }
    assert(denied); // Entire access, including its end, must be mapped.
    context.Call(helper,{0x80000000,0x34,sizeof(output)}); assert(output==0x34343434);
    denied=false;
    try { context.Call(context.Helper("_panic"),{}); } catch(const std::exception &) { denied=true; }
    assert(denied);
    std::uint32_t block[]={0x52800020,0xd65f03c0};
    context.Map(0x90000000,{reinterpret_cast<std::uint8_t*>(block),sizeof(block)},true,true);
    assert(context.Call(0x90000000,{})==1);
    const std::uint32_t replacement[]={0x52800040,0xd65f03c0};
    const auto replacementAddress=context.Add(replacement);
    context.Call(context.Helper("memcpy"),{0x90000000,replacementAddress,sizeof(replacement)});
    context.Call(0x10290,{0x90000000});
    assert(context.Call(0x90000000,{})==2);
    unsigned polls{};
    PluginContext cancellable([&]{return ++polls>=3;});cancellable.Load(image);
    denied=false;try {cancellable.Call(0x10280,{});}catch(const std::exception &){denied=true;}
    assert(denied && polls>=3);
    std::cout<<"plugin ARM64: execution, stack args, shared writes, helpers, bounds and panic passed\n";
}
