// SPDX-License-Identifier: MPL-2.0
// Compile complete production entry points. Inject reconciliation at every
// preparation step and require stack locks to unwind before submission/retry.
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <utility>
using u8=uint8_t;using u32=uint32_t;
template<class T> using span=std::span<T>;
struct FenceCycle {};
namespace vk {
enum class PipelineStageFlagBits {eTransfer=1,eAllCommands=2};
enum class AccessFlagBits {eTransferWrite=1,eMemoryRead=2,eMemoryWrite=4,eTransferRead=8};
int operator|(AccessFlagBits a,AccessFlagBits b){return int(a)|int(b);}
struct BufferCopy {size_t size,srcOffset,dstOffset;};
struct AccessFlags {int value;AccessFlags(AccessFlagBits v):value(int(v)){}AccessFlags(int v):value(v){}};
struct MemoryBarrier {AccessFlags srcAccessMask,dstAccessMask;};
namespace raii {struct CommandBuffer {
void copyBuffer(int,int,BufferCopy){}
void fillBuffer(int,size_t,size_t,u32){}
void pipelineBarrier(PipelineStageFlagBits,PipelineStageFlagBits,int,MemoryBarrier,std::initializer_list<int>,std::initializer_list<int>){}
};}
}
struct Retry {};
struct Harness {
    unsigned step{},failAt{},localLocks{},submissions{},commands{},completed{};
    void Prepare(){if(++step==failAt)throw Retry{};}
} state;
template<class T> struct ContextLock {
    bool owns=true;
    ContextLock(uint64_t,T&){++state.localLocks;}
    ContextLock(ContextLock&& other):owns(other.owns){other.owns=false;}
    ~ContextLock(){if(owns)--state.localLocks;}
    void Release(){if(owns){--state.localLocks;owns=false;}}
};
struct GPU;
struct Buffer {
    void BlockAllCpuBackingWrites(){}
    void BlockSequencedCpuBackingWrites(){}
    void MarkGpuDirty(int&){state.Prepare();}
};
struct Binding {int buffer{};size_t offset{},size{};};
struct BufferView {
    std::shared_ptr<Buffer> buffer=std::make_shared<Buffer>();size_t size=16;
    Buffer *GetBuffer(){return buffer.get();}
    Binding GetBinding(GPU&)const{return {};}
    void Write(span<u8>,size_t,int&,const std::function<void()>& callback){state.Prepare();callback();}
    void CopyFrom(BufferView&,int&,const std::function<void()>& callback){state.Prepare();callback();}
};
struct Manager {
    BufferView FindOrCreate(span<u8>,uint64_t,auto&&){state.Prepare();return {};}
};
struct Executor {
    uint64_t tag{1};int usageTracker{};std::shared_ptr<FenceCycle> cycle;
    template<class Op> void RunWithTextureBackingRetry(Op&& op){
        for(;;)try{op();return;}catch(const Retry&){
            assert(state.localLocks==0 && state.commands==0);
            ++state.submissions;state.failAt=0;
        }
    }
    void AttachLockedBuffer(std::shared_ptr<Buffer>,ContextLock<Buffer>&& lock){state.Prepare();lock.Release();}
    void AttachLockedBufferView(BufferView&,ContextLock<BufferView>&& lock){state.Prepare();lock.Release();}
    void AttachBuffer(BufferView&){state.Prepare();}
    void AddOutsideRpCommand(auto&&){++state.commands;}
};
struct GPU {Manager buffer;struct Allocator {Binding Push(std::shared_ptr<FenceCycle>,span<u8>){return {};}} megaBufferAllocator;};
namespace util {bool IsAligned(size_t value,size_t alignment){return value%alignment==0;}}
struct exception:std::exception{explicit exception(const char*){}};
struct Inline2Memory {GPU &gpu;Executor &executor;void UploadSingleMapping(span<u8>,span<u8>);};
struct MaxwellDma {GPU &gpu;Executor &executor;void Copy(span<u8>,span<u8>);void Clear(span<u8>,u32);};
namespace engine {enum class ShaderStage {Vertex};}
struct Context {Executor executor;};
struct Maxwell3D {
    Context ctx;
    struct Constants {
        void Load(Context&,span<u32>,u32){state.Prepare();++state.completed;}
        void Bind(Context&,engine::ShaderStage,u32){state.Prepare();++state.completed;}
        void Unbind(engine::ShaderStage,u32){++state.completed;}
    } constantBuffers;
    void LoadConstantBuffer(span<u32>,u32);
    void BindConstantBuffer(engine::ShaderStage,u32,bool);
};
#include "boundaries.inc"
void Check(const std::function<void()>& operation,bool command){
    state={};operation();const auto steps=state.step;assert(steps>0);
    for(unsigned fail=1;fail<=steps;++fail){
        state={};state.failAt=fail;operation();
        assert(state.submissions==1 && state.localLocks==0);
        assert(state.commands==unsigned(command));
        assert(state.completed==unsigned(!command));
    }
}
int main(){
    GPU gpu;Executor executor;Inline2Memory inlineMemory{gpu,executor};MaxwellDma dma{gpu,executor};Maxwell3D maxwell;
    u8 source[16]{},destination[16]{};u32 constants[4]{};
    Check([&]{inlineMemory.UploadSingleMapping(destination,source);},true);
    Check([&]{dma.Copy(destination,source);},true);
    Check([&]{dma.Clear(destination,0);},true);
    Check([&]{maxwell.LoadConstantBuffer(constants,0);},false);
    Check([&]{maxwell.BindConstantBuffer(engine::ShaderStage::Vertex,0,true);},false);
    state={};maxwell.BindConstantBuffer(engine::ShaderStage::Vertex,0,false);assert(state.completed==1);
    std::cout<<"GPU buffer operation retry boundaries passed\n";
}
