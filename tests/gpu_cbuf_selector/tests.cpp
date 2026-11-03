// SPDX-License-Identifier: MPL-2.0
#include <common/mapping_identity.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <source_location>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using i32=int32_t;
template<class T> struct span:std::span<T> {using std::span<T>::span;span(std::span<T> v):std::span<T>(v){}bool valid() const{return this->data()!=nullptr;}};
template<class...T> std::runtime_error exception(const char *s,T&&...){return std::runtime_error(s);}
std::vector<std::string> logs;
bool failLog{};
template<class...T> void Log(const char *format,T&&...values) {
    if(failLog)throw std::bad_alloc{};
    std::ostringstream out;out<<format;((out<<' '<<values),...);logs.push_back(out.str());
}
#define LOGE(...) Log(__VA_ARGS__)
#define LOGW(...) Log(__VA_ARGS__)
#define LOGI(...) Log(__VA_ARGS__)
#define TRACE_EVENT(...)
#include "gmmu.inc"
namespace skyline::gpu {
struct Buffer;
struct BufferView {Buffer *buffer{};u8 *bytes{};size_t size{};operator bool(){return buffer!=nullptr;}Buffer *GetBuffer(){return buffer;}};
struct Buffer {span<u8> bytes;BufferView TryGetView(span<u8> part){return {this,part.data(),part.size()};}};
template<class T>struct ContextLock {};
struct BufferManager {
    std::vector<std::shared_ptr<Buffer>> buffers;
    BufferView FindOrCreate(span<u8> bytes,u64,std::function<void(std::shared_ptr<Buffer>,ContextLock<Buffer>&&)>) {
        auto b=std::make_shared<Buffer>(Buffer{bytes});buffers.push_back(b);return {b.get(),bytes.data(),bytes.size()};
    }
};
namespace interconnect {
struct CommandExecutor {u64 tag{1};void AttachLockedBuffer(std::shared_ptr<Buffer>,ContextLock<Buffer>&&) {}};
struct InterconnectContext {
    struct As{translation_test::Gmmu gmmu;};
    struct{std::shared_ptr<As> asCtx{std::make_shared<As>()};u64 channelSequenceNumber{7};}channelCtx;
    CommandExecutor executor;
    struct {BufferManager buffer;}gpu;
};
#include "cache_class.inc"
#include "cache.inc"
namespace dirty {
struct ManualDirty{};struct CachedManualDirty:ManualDirty{};struct RefreshableManualDirty:ManualDirty{};
struct Handle {bool *value;};
struct Manager {template<class T>void Bind(Handle,T&&) {}};
template<class T>struct BoundSubresource {T value;template<class M>BoundSubresource(M&,Handle,const T&t):value(t){}const T *operator->()const{return &value;}};
#include "dirty.inc"
}
using DirtyManager=dirty::Manager;
namespace maxwell3d {
namespace engine {struct ConstantBufferSelector{u32 size{};u64 address{};};enum class ShaderStage{Vertex};inline constexpr size_t ShaderStageCount=1,ShaderStageConstantBufferCount=2;}
#include "selector_class.inc"
struct ConstantBuffer{BufferView view;};
using ConstantBufferSet=std::array<std::array<ConstantBuffer,2>,1>;
#include "buffers_class.inc"
#include "selector.inc"
}
}}
namespace skyline::soc::gm20b::engine {
struct MacroEngineBase {
    std::function<void(u32,u32)> send;
    void CallMethodFromMacro(u32 method,u32 value){send(method,value);}
    std::function<u32(u32)> read{};
    u32 ReadMethodFromMacro(u32 method){return read ? read(method) : 0;}
};
#include "macro_class.inc"
}
#include "macro.inc"
void ReplayCapturedCastleMacro() {
    using namespace skyline::gpu::interconnect;
    using namespace maxwell3d;
    using namespace skyline::soc::gm20b::engine;
    constexpr u64 address=0x405E31400;
    // All instructions through the failing Send are contiguous in the captured
    // entryCode/pcCode records. No guessed instruction or argument is needed.
    const std::array<u32,23> words{0x3470A15,0x401,0x4A210,0x55007,0x301,0x2004211,0x21005212,0x61801022,
        0x1841,0x3470A11,0x61801022,0x2041,0x3488A11,0x61801022,0x1A31,0x6380151,0x40000041,
        0x2304042,0x46004042,0x19C08112,0x2410911,0x618008A2,0x44041};
    std::array<u32,256> code{};std::copy(words.begin(),words.end(),code.begin()+115);
    std::array<u32,5> args{1,0x692F,0x5E30030,0,0x405E314};
    for(bool resident:{false,true})for(unsigned purges:{0U,3U}) {
        InterconnectContext ctx;auto &memory=ctx.channelCtx.asCtx->gmmu;memory.blocks={{0,nullptr,{}}};
        DirtyManager manager;engine::ConstantBufferSelector selector{};ConstantBuffers buffers{manager,{selector}};
        std::vector<u8> bytes(0x10000);for(size_t i{};i<bytes.size();++i)bytes[i]=static_cast<u8>((i*11+i/29)%251);
        if(resident)memory.Map(address,bytes.data(),bytes.size());
        const auto revision=memory.MappingRevision();
        for(unsigned i{};i<purges;++i)buffers.MarkAllDirty();
        bool sentBind{},failed{};
        MacroEngineBase target{[&](u32 method,u32 value) {
            if(method==0x8E0){selector.size=value;buffers.MarkAllDirty();}
            if(method==0x8E1){selector.address=(selector.address&0xFFFFFFFFULL)|(static_cast<u64>(value)<<32);buffers.MarkAllDirty();}
            if(method==0x8E2){selector.address=(selector.address&0xFFFFFFFF00000000ULL)|value;buffers.MarkAllDirty();}
            if(method==0x904){
                assert(value==0x11 && selector.address==address && selector.size==0x10000);sentBind=true;
                buffers.Bind(ctx,engine::ShaderStage::Vertex,1);
            }
        }};
        // The captured path implies a nonzero result for mask & 0x692F. Its
        // exact prior register value is absent; 1 chooses that observed path.
        target.read=[](u32 method){assert(method==0xD1D);return 1;};
        MacroInterpreter interpreter{span<u32>{code}};
        try{interpreter.Execute(115,span<u32>{args},&target);}catch(const std::runtime_error &e){failed=std::string(e.what())=="Constant buffer selector is not mapped";}
        assert(sentBind && failed==!resident && memory.MappingRevision()==revision);
        if(resident){const auto view=buffers.boundConstantBuffers[0][1].view;assert(view.size==bytes.size() && std::equal(bytes.begin(),bytes.end(),view.bytes));}
    }
    std::puts("PASS: captured MME words/arguments generate exact selector/bind; resident bytes succeed and absent mappings reject with/without purges");
}
int main() {
    if(std::getenv("CBUF_CASTLE_REPLAY_REF")){ReplayCapturedCastleMacro();return 0;}
    using namespace skyline::gpu::interconnect;
    using namespace maxwell3d;
    InterconnectContext ctx;auto &gmmu=ctx.channelCtx.asCtx->gmmu;gmmu.blocks={{0,nullptr,{}}};
    DirtyManager manager;engine::ConstantBufferSelector selector{65536,0x1000};ConstantBuffers buffers{manager,{selector}};
    std::vector<u8> bytes(4096);for(size_t i{};i<bytes.size();++i)bytes[i]=static_cast<u8>(i*7+3);
    gmmu.Map(0x1000,bytes.data(),bytes.size());
    buffers.Bind(ctx,engine::ShaderStage::Vertex,0);
    auto &bound=buffers.boundConstantBuffers[0][0].view;
    assert(bound.size==4096 && std::equal(bytes.begin(),bytes.end(),bound.bytes));
    assert(logs.empty());
    for(unsigned i{};i<12;++i) {buffers.MarkAllDirty();buffers.Bind(ctx,engine::ShaderStage::Vertex,0);}
    assert(logs.empty()); // The bounded history is quiet on successful binds.
    // Preserve the known upper-bound clamping behavior and explicit purge path.
    gmmu.Unmap(0x1000,4096);buffers.MarkAllDirty();
    const auto revision=gmmu.MappingRevision();const auto boundBytes=bound.bytes;const auto beforeAlloc=ctx.gpu.buffer.buffers.size();
    bool failed{};try{buffers.Bind(ctx,engine::ShaderStage::Vertex,1);}catch(const std::runtime_error &e){failed=std::string(e.what())=="Constant buffer selector is not mapped";}
    assert(failed && bound.bytes==boundBytes && ctx.gpu.buffer.buffers.size()==beforeAlloc && gmmu.MappingRevision()==revision);
    assert(std::any_of(logs.begin(),logs.end(),[](const auto&s){return s.find("CBUF_DIAG bind")!=std::string::npos;}));
    assert(std::count_if(logs.begin(),logs.end(),[](const auto&s){return s.find("CBUF_DIAG history")!=std::string::npos;})==8);
    assert(gmmu.mappingIdentity.Inspect(0x1000).generation==0); // Diagnosis must not mint a receipt.
    // Logging must not replace the original selector exception.
    failLog=true;failed=false;buffers.MarkAllDirty();
    try{buffers.Bind(ctx,engine::ShaderStage::Vertex,1);}catch(const std::runtime_error &e){failed=std::string(e.what())=="Constant buffer selector is not mapped";}
    failLog=false;assert(failed);
    // A real interpreter Send reaches the same production Bind; the exception
    // must survive with a bounded macro PC/method/argument diagnostic.
    using namespace skyline::soc::gm20b::engine;
    std::array<u32,4> code{1U | (6U<<4) | (0x900U<<14) | (1U<<7),0,0,0};
    std::array<u32,2> args{0,0x11};
    MacroInterpreter interpreter{span<u32>{code}};
    MacroEngineBase target{[&](u32 method,u32 argument) {
        assert(method==0x900 && argument==0x11);
        buffers.MarkAllDirty();buffers.Bind(ctx,engine::ShaderStage::Vertex,1);
    }};
    failed=false;
    try {interpreter.Execute(0,span<u32>{args},&target);} catch(const std::runtime_error &e){failed=std::string(e.what())=="Constant buffer selector is not mapped";}
    assert(failed);
    assert(std::any_of(logs.begin(),logs.end(),[](const auto&s){return s.find("MME_DIAG failure")!=std::string::npos;}));
    assert(std::any_of(logs.begin(),logs.end(),[](const auto&s){return s.find("MME_DIAG failure")!=std::string::npos && s.find("2304 17")!=std::string::npos;}));
    const auto emitted=logs.size();
    failLog=true;
    try {interpreter.Execute(0,span<u32>{args},&target);} catch(const std::runtime_error &e){assert(std::string(e.what())=="Constant buffer selector is not mapped");}
    failLog=false;
    for(unsigned i{};i<3;++i)try{interpreter.Execute(0,span<u32>{args},&target);}catch(const std::runtime_error &){}
    assert(logs.size()==emitted); // At most two failed-selector and macro reports per process.
    // Read-only identity inspection neither creates generations nor changes revisions.
    skyline::MappingIdentity identity;const auto initial=identity.Revision();
    assert(identity.Inspect(20).generation==0 && identity.Revision()==initial);
    auto tracked=identity.Capture(10,20);const auto observed=identity.Inspect(20);
    assert(observed.generation==tracked.GenerationAt(20) && observed.begin==10 && observed.end==30);
    {auto mutation=identity.Invalidate(20,1);}
    assert(identity.Inspect(20).generation==0 && identity.Inspect(19).generation==observed.generation);
    // Explicit purge/reprepare across P->Q uses Q's independent bytes. No diagnostic repair.
    std::vector<u8> replacement(4096,0x72);gmmu.Map(0x1000,replacement.data(),replacement.size());
    buffers.MarkAllDirty();buffers.Bind(ctx,engine::ShaderStage::Vertex,1);
    const auto repaired=buffers.boundConstantBuffers[0][1].view;
    assert(repaired.size==4096 && std::equal(replacement.begin(),replacement.end(),repaired.bytes));
    assert(std::equal(bytes.begin(),bytes.end(),boundBytes));
    // A partially resident upper bound still clamps to the resident prefix.
    selector.size=8192;buffers.MarkAllDirty();buffers.Bind(ctx,engine::ShaderStage::Vertex,1);
    assert(buffers.boundConstantBuffers[0][1].view.size==4096);
    // A sparse placeholder remains nonresident, never a fabricated empty buffer.
    gmmu.Map(0x1000,reinterpret_cast<u8 *>(uintptr_t{0xCAFEBABE}),4096,{true});buffers.MarkAllDirty();
    failed=false;try{buffers.Bind(ctx,engine::ShaderStage::Vertex,1);}catch(const std::runtime_error &){failed=true;}
    assert(failed && std::equal(replacement.begin(),replacement.end(),repaired.bytes));
    // A larger minimum updates the selector cache, with upper-bound clamping unchanged.
    gmmu.Map(0x1000,bytes.data(),bytes.size());selector.size=32;
    ConstantBufferSelectorState state{{nullptr},manager,{selector}};
    state.Flush(ctx,64);assert(state.view->size==64);
    state.Refresh(ctx,128);assert(state.view->size==128);
    assert(std::equal(bytes.begin(),bytes.begin()+128,state.view.view.bytes));
    ReplayCapturedCastleMacro();
    std::puts("PASS: production selector/cache data, clamp, unmapped rejection, bounded diagnosis and unchanged exception");
}
