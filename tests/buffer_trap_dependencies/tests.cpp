// SPDX-License-Identifier: MPL-2.0
#include "fixture.h"
#include <future>
#include <chrono>
using namespace std::chrono_literals;
namespace skyline {using ContextTag=uint64_t;}
// Buffer::unlock is also extracted verbatim; ContextTag is its host scalar facade.
static void WaitForPrepare(const std::shared_ptr<Buffer>& buffer) {buffer->lockCalls.wait(0);}
static void CleanStoreDoesNotWait() {
    Fixture f;auto b=f.buffer;
    std::memset(f.mirror,0x55,4096);std::memset(b->backing->data(),0x55,4096);
    auto reader=std::make_shared<gpu::FenceCycle>();b->cycle=reader;
    std::promise<void> release;auto gate=release.get_future().share();reader->complete=[gate]{gate.wait();};
    b->mutex.lock();b->blocked=true;
    f.process.trap.TrapRegions(*b->trapHandle,true);
    auto store=std::async(std::launch::async,[&]{f.Write(37,0x9a);});
    WaitForPrepare(b);
    b->unlock(); // Real production unlock drops AllWrites before releasing the resource.
    const bool independent=store.wait_for(200ms)==std::future_status::ready;
    if(!independent){release.set_value();store.get();Check(false,"CPU mirror store waited for a pending GPU reader");}
    store.get();
    Check(reader->waits==0&&b->cycle==reader,"mirror store consumed/discarded reader cycle");
    Check(b->backing->data()[37]==0x55&&f.mirror[37]==0x9a,"mirror store changed in-flight GPU backing");
    auto upload=std::async(std::launch::async,[&]{std::scoped_lock lock{*b};b->SynchronizeHost();});
    const bool retained=upload.wait_for(100ms)==std::future_status::timeout;
    release.set_value();upload.get();
    Check(retained&&reader->waits==1,"real backing upload did not wait for GPU reader");
    for(size_t i{};i<4096;++i)Check(b->backing->data()[i]==(i==37?0x9a:0x55),"partial upload lost untouched bytes");
    std::puts("PASS clean mirror store proceeds; GPU backing remains unchanged; later upload retains reader wait");
}
static void DirtyProducerStillWaits() {
#if STRATO_BUFFER_READBACK_PROBE
    namespace bp=gpu::buffer_readback_probe;
    const auto waitsBefore=bp::Read(bp::totals[bp::DirtyPrepare]);
    const auto downloadsBefore=bp::Read(bp::totals[bp::Download]);
#endif
    Fixture f;auto b=f.buffer;auto producer=f.Pending(true);
    std::promise<void> release,entered;auto gate=release.get_future().share();
    producer->complete=[&]{entered.set_value();gate.wait();std::memset(b->backing->data(),0x64,4096);};
    auto store=std::async(std::launch::async,[&]{f.Write(37,0x9a);});
    entered.get_future().wait();const bool retained=store.wait_for(100ms)==std::future_status::timeout;
    release.set_value();store.get();Check(retained&&producer->waits==1,"mandatory producer dependency skipped");
    Check(f.mirror[0]==0x64&&f.mirror[37]==0x9a,"GPU-dirty preservation/readback changed");
#if STRATO_BUFFER_READBACK_PROBE
    const auto waitsAfter=bp::Read(bp::totals[bp::DirtyPrepare]);
    const auto downloadsAfter=bp::Read(bp::totals[bp::Download]);
    Check(waitsAfter.calls==waitsBefore.calls+1&&waitsAfter.ns>waitsBefore.ns,"producer wait attribution missing");
    Check(downloadsAfter.calls==downloadsBefore.calls+1&&downloadsAfter.bytes==downloadsBefore.bytes+4096,"mandatory download attribution missing");
#endif
    std::puts("PASS GPU-dirty producer wait and mandatory readback retained");
}
static void ProducerFailureRecovery() {
    Fixture f;auto b=f.buffer;auto producer=f.Pending(true);
    producer->complete=[]{throw std::runtime_error("producer failed");};
    bool failed{};try{f.process.trap.HandleTrap(f.guest,true);}catch(const std::runtime_error&){failed=true;}
    Check(failed&&b->cycle==producer&&b->dirtyState==Buffer::DirtyState::GpuDirty,"failed producer lost authority/dependency");
    producer->complete=[&]{std::memset(b->backing->data(),0x64,4096);};
    Check(f.process.trap.HandleTrap(f.guest,true),"producer retry failed");
    f.Write(37,0x9a);Check(f.mirror[0]==0x64&&f.mirror[37]==0x9a,"producer recovery lost preserved data");
    std::puts("PASS failed producer retains GPU authority/cycle; retry restores data before partial store");
}
static void AnnotationDoesNotSynchronize() {
    Fixture f;auto b=f.buffer;auto producer=f.Pending(true);
    {std::scoped_lock lock{*b};b->DiagnoseGpuWrite(gpu::buffer_readback_probe::WriteSource::Query, 32, 16, producer);}
    Check(b->cycle==producer&&producer->waits==0&&b->dirtyState==Buffer::DirtyState::GpuDirty,
          "diagnostic annotation changed data authority or waited");
#if STRATO_BUFFER_READBACK_PROBE
    Check(b->diagnosticLastAnnotation.cycle==producer->GetDiagnosticId(),"annotation cycle identity lost");
    Check(!gpu::buffer_readback_probe::Find(1,4096,false),"first annotation incorrectly admitted resource");
#endif
    f.Write(37,0x9a);
#if STRATO_BUFFER_READBACK_PROBE
    auto entry=gpu::buffer_readback_probe::Find(1,4096,false);
    Check(entry&&entry->sampledSource.load()==static_cast<unsigned>(gpu::buffer_readback_probe::WriteSource::Query)
          &&entry->sampledOffset.load()==32&&entry->sampledSize.load()==16,
          "pre-admission binding snapshot missing from first sampled wait");
    Check(gpu::buffer_readback_probe::Read(entry->writes[static_cast<unsigned>(gpu::buffer_readback_probe::WriteSource::Query)]).calls==0,
          "retaining initial annotation invented another write event");
#endif
    std::puts("PASS production write annotation preserves cycle, dirty state and wait policy");
}
#ifdef WITH_VULKAN
#include "vulkan.inc"
static void NativePendingReader() {
    Vulkan vk;NativeBuffer backing(vk),output(vk);Fixture f;auto b=f.buffer;
    b->backing->external=backing.data;std::memset(backing.data,0x55,4096);std::memset(f.mirror,0x55,4096);
    VkEvent event{};VkEventCreateInfo ec{};ec.sType=VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    VkCheck(vkCreateEvent(vk.device,&ec,nullptr,&event));
    auto cmd=vk.Begin();
    vkCmdWaitEvents(cmd,1,&event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,nullptr,0,nullptr,0,nullptr);
    VkBufferCopy copy{0,0,4096};vkCmdCopyBuffer(cmd,backing.buffer,output.buffer,1,&copy);
    Barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    auto fence=vk.Submit(cmd);
    auto reader=std::make_shared<gpu::FenceCycle>();b->cycle=reader;
    reader->complete=[&]{VkCheck(vkWaitForFences(vk.device,1,&fence,true,UINT64_MAX));};
    b->mutex.lock();b->blocked=true;f.process.trap.TrapRegions(*b->trapHandle,true);
    auto store=std::async(std::launch::async,[&]{f.Write(37,0x9a);});
    WaitForPrepare(b);b->unlock();
    const bool independent=store.wait_for(200ms)==std::future_status::ready;
    if(!independent){vkSetEvent(vk.device,event);store.get();vk.Retire(cmd,fence);vkDestroyEvent(vk.device,event,nullptr);Check(false,"native mirror store waited for read-only GPU consumer");}
    store.get();
    const bool untouched=backing.data[37]==0x55;
    const bool pending=vkGetFenceStatus(vk.device,fence)==VK_NOT_READY;
    auto upload=std::async(std::launch::async,[&]{std::scoped_lock lock{*b};b->SynchronizeHost();});
    const bool blocked=upload.wait_for(100ms)==std::future_status::timeout;
    VkCheck(vkSetEvent(vk.device,event));upload.get();vk.Retire(cmd,fence);vkDestroyEvent(vk.device,event,nullptr);
    Check(untouched&&pending&&blocked&&reader->waits==1,"native backing upload failed to preserve pending reader");
    for(size_t i{};i<4096;++i)Check(output.data[i]==0x55,"in-flight GPU consumer saw later CPU store");
    cmd=vk.Begin();Barrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyBuffer(cmd,backing.buffer,output.buffer,1,&copy);
    Barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    fence=vk.Submit(cmd);vk.Retire(cmd,fence);
    for(size_t i{};i<4096;++i)Check(output.data[i]==(i==37?0x9a:0x55),"subsequent GPU consumer lost partial CPU store");
    Check(!validationErrors,"native Vulkan validation errors");
    std::puts("PASS native pending GPU reader sees original bytes; CPU mirror store independent; upload waits; next GPU reader sees updated bytes; zero Vulkan validation errors");
}

static void NativePendingWriter() {
    Vulkan vk;NativeBuffer backing(vk);Fixture f;auto b=f.buffer;b->backing->external=backing.data;
    VkEvent event{};VkEventCreateInfo ec{};ec.sType=VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;VkCheck(vkCreateEvent(vk.device,&ec,nullptr,&event));
    auto cmd=vk.Begin();vkCmdWaitEvents(cmd,1,&event,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,nullptr,0,nullptr,0,nullptr);
    vkCmdFillBuffer(cmd,backing.buffer,0,4096,0x64646464);
    Barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    auto fence=vk.Submit(cmd);auto producer=std::make_shared<gpu::FenceCycle>();b->cycle=producer;b->dirtyState=Buffer::DirtyState::GpuDirty;
    producer->complete=[&]{VkCheck(vkWaitForFences(vk.device,1,&fence,true,UINT64_MAX));};
    f.process.trap.TrapRegions(*b->trapHandle,false);
    auto store=std::async(std::launch::async,[&]{f.Write(37,0x9a);});
    const bool retained=store.wait_for(100ms)==std::future_status::timeout;
    VkCheck(vkSetEvent(vk.device,event));store.get();vk.Retire(cmd,fence);vkDestroyEvent(vk.device,event,nullptr);
    Check(retained&&producer->waits==1,"native GPU writer dependency skipped");
    for(size_t i{};i<4096;++i)Check(f.mirror[i]==(i==37?0x9a:0x64),"native required readback lost data");
    Check(!validationErrors,"native writer validation errors");
    std::puts("PASS native pending GPU writer blocks CPU store; mandatory GPU-dirty readback preserves bytes; zero validation errors");
}

#endif
int main(){
    struct sigaction action{};action.sa_sigaction=Fault;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
    Check(!sigaction(SIGSEGV,&action,nullptr),"sigaction failed");
    try{AnnotationDoesNotSynchronize();CleanStoreDoesNotWait();DirtyProducerStillWaits();ProducerFailureRecovery();
#ifdef WITH_VULKAN
    NativePendingReader();NativePendingWriter();
#endif
    }catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
