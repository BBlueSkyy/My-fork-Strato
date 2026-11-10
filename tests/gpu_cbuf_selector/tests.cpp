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
template<class...T> void Log(const char *format,T&&...values) {
    std::ostringstream out;out<<format;((out<<' '<<values),...);logs.push_back(out.str());
}
#define LOGE(...) Log(__VA_ARGS__)
#define LOGW(...) Log(__VA_ARGS__)
#define LOGI(...) Log(__VA_ARGS__)
#define TRACE_EVENT(...)
#include "gmmu.inc"
namespace skyline::gpu {
struct Buffer;
struct BufferView {Buffer *buffer{};u8 *bytes{};size_t size{};operator bool(){return buffer!=nullptr;}Buffer *GetBuffer(){return buffer;}void Read(bool,const std::function<void()> &,span<u8> dst,size_t offset){std::copy_n(bytes+offset,dst.size(),dst.data());}};
struct Buffer {span<u8> bytes;BufferView TryGetView(span<u8> part){return {this,part.data(),part.size()};}void RequireRegionCoherence(u64){}};
template<class T>struct ContextLock {ContextLock(u64,T&){}bool IsFirstUsage()const{return true;}};
template<class T>ContextLock(u64,T&) -> ContextLock<T>;
struct BufferManager {
    std::vector<std::shared_ptr<Buffer>> buffers;
    BufferView FindOrCreate(span<u8> bytes,u64,std::function<void(std::shared_ptr<Buffer>,ContextLock<Buffer>&&)>) {
        auto b=std::make_shared<Buffer>(Buffer{bytes});buffers.push_back(b);return {b.get(),bytes.data(),bytes.size()};
    }
};
namespace interconnect {
struct TextureAliasReconciliationRequest {};
struct GpuDirtyConstantBufferReadRequest {};
struct CommandExecutor {
    u64 submissionNumber{};u64 tag{1};
    bool regionOperationActive{};
    skyline::MappingIdentity::Epoch textureMappingEpoch;
    void StartTextureMapping(const skyline::MappingIdentity::Epoch &epoch);
    void AttachLockedBuffer(std::shared_ptr<Buffer>,ContextLock<Buffer>&&) {}
};
#include "buffer_epoch.inc"
struct InterconnectContext {
    struct As{translation_test::Gmmu gmmu;};
    struct{std::shared_ptr<As> asCtx{std::make_shared<As>()};u64 channelSequenceNumber{7};}channelCtx;
    CommandExecutor executor;
    struct {BufferManager buffer;}gpu;
};
#include "cache_class.inc"
#include "cache.inc"
#include "constant_buffer_class.inc"
#include "constant_buffer.inc"
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
void CheckBufferMappingEpoch() {
    using namespace skyline::gpu::interconnect;
    InterconnectContext ctx;auto &gmmu=ctx.channelCtx.asCtx->gmmu;gmmu.blocks={{0,nullptr,{}}};
    std::vector<u8> p(4096),q(4096);
    for(size_t i{};i<p.size();++i){p[i]=static_cast<u8>(i*13+7);q[i]=static_cast<u8>(i*29+19);}
    CachedMappedBufferView cache;
    ctx.executor.regionOperationActive=true;
    auto next=[&]{ctx.executor.textureMappingEpoch={};};
    gmmu.Map(0x1000,p.data(),p.size());next();cache.Update(ctx,0x1000,4096);
    const auto old=cache.view;const auto epoch=gmmu.ReadMappingEpoch();
    const auto allocations=ctx.gpu.buffer.buffers.size();
    for(size_t i{};i<1000;++i)cache.Update(ctx,0x1000,4096);
    assert(ctx.gpu.buffer.buffers.size()==allocations);
    // No explicit purge, no changed selector registers, same address and size.
    gmmu.Map(0x1000,q.data(),q.size());next();cache.Update(ctx,0x1000,4096);
    assert(cache.view.bytes==q.data() && std::equal(q.begin(),q.end(),cache.view.bytes));
    assert(std::equal(p.begin(),p.end(),old.bytes) && !epoch.SameVersion(gmmu.ReadMappingEpoch()));
    assert(ctx.executor.textureMappingEpoch.SameVersion(gmmu.ReadMappingEpoch()));
    gmmu.Map(0x1000,p.data(),p.size());next();cache.Update(ctx,0x1000,4096);
    assert(cache.view.bytes==p.data());
    gmmu.Unmap(0x1000,4096);next();cache.Update(ctx,0x1000,4096);assert(!cache.view);
    gmmu.Map(0x1000,q.data(),q.size());next();cache.Update(ctx,0x1000,8192,false);
    assert(cache.view.bytes==q.data() && cache.view.size==4096);
    const auto unchanged=gmmu.ReadMappingEpoch();gmmu.Map(0x1000,q.data(),q.size());
    cache.Update(ctx,0x1000,8192,false);assert(unchanged.SameVersion(ctx.executor.textureMappingEpoch));
    InterconnectContext other;auto &otherGmmu=other.channelCtx.asCtx->gmmu;
    otherGmmu.blocks={{0,nullptr,{}}};otherGmmu.Map(0x1000,p.data(),p.size());
    cache.Update(other,0x1000,4096);assert(cache.view.bytes==p.data());
    cache.Update(ctx,0x1000,4096);assert(cache.view.bytes==q.data());
    // Same-size selector refresh must validate its cached translation too.
    using namespace maxwell3d;
    DirtyManager manager;engine::ConstantBufferSelector selector{4096,0x1000};
    ConstantBufferSelectorState state{{nullptr},manager,{selector}};state.Flush(ctx,0);
    gmmu.Map(0x1000,p.data(),p.size());next();state.Refresh(ctx,0);assert(state.view.view.bytes==p.data());
    // Remapping after acquisition rejects the lease and same-attempt replay.
    next();cache.Update(ctx,0x1000,4096);
    gmmu.Map(0x1000,q.data(),q.size());
    assert(!ctx.executor.textureMappingEpoch.Acquire());
    bool rejected{};
    try{cache.Update(ctx,0x1000,4096);}catch(const TextureAliasReconciliationRequest &){rejected=true;}
    assert(rejected);
    next();cache.Update(ctx,0x1000,4096);assert(cache.view.bytes==q.data());
    std::puts("PASS: cached buffer and selector epoch revalidation without purge, P->Q->P, unmapped/recovery and stable no-op reuse");
}
// The descriptor's final binding API is a host boundary; count the dummy
// fallback to prove the actual full/quick/compute snippets reject stale bindings.
namespace vk { enum class PipelineStageFlagBits { eComputeShader }; }
namespace skyline::gpu::interconnect {
unsigned descriptorDummies{};
BufferView GetConstantBufferBinding(InterconnectContext &, span<const u32>, BufferView view,
                                    size_t, auto, u32 &, u32 &) {
    if (!view) ++descriptorDummies;
    return view;
}
BufferView ConsumeDescriptor(InterconnectContext &ctx, ConstantBuffer &bound, unsigned path) {
    size_t i{},cbufIdx{};u32 srcStageMask{},dstStageMask{};
    span<ConstantBuffer> stageConstantBuffers{&bound,1};
    std::array<span<ConstantBuffer>,1> matrix{{stageConstantBuffers}};
    struct Fields { std::array<u32,1> constantBufferUsedSizes{{16}};u32 stage{}; } stage,stageDescInfo;
    struct ShaderStage { struct Info { std::array<u32,1> constant_buffer_used_sizes{{16}}; } info; } shaderStage;
    switch(path) {
        case 0: { auto &constantBuffers=matrix;
#include "descriptor0.inc"
        }
        case 1: {
#include "descriptor1.inc"
        }
        case 2: { auto &constantBuffers=stageConstantBuffers;
#include "descriptor2.inc"
        }
        default: throw std::logic_error("invalid descriptor test path");
    }
}
}
void CheckBoundDescriptorConsumption() {
    using namespace skyline::gpu::interconnect;
    using namespace maxwell3d;
    for(unsigned path{};path<3;++path) {
        InterconnectContext ctx;auto &gmmu=ctx.channelCtx.asCtx->gmmu;gmmu.blocks={{0,nullptr,{}}};
        std::vector<u8> p(4096,0x19),q(4096,0x72);gmmu.Map(0x1000,p.data(),p.size());
        DirtyManager manager;engine::ConstantBufferSelector selector{4096,0x1000};ConstantBuffers buffers{manager,{selector}};
        buffers.Bind(ctx,engine::ShaderStage::Vertex,0);auto &bound=buffers.boundConstantBuffers[0][0];
        assert(ConsumeDescriptor(ctx,bound,path).bytes==p.data());
        gmmu.Unmap(0x1000,p.size());buffers.RefreshMappings();assert(!bound.view);
        const auto before=descriptorDummies;
        bool rejected{};try { ConsumeDescriptor(ctx,bound,path); }
        catch(const std::runtime_error &e) { rejected=std::string(e.what())=="Bound constant buffer is not mapped"; }
        assert(rejected && descriptorDummies==before);
        gmmu.Map(0x1000,q.data(),q.size());buffers.RefreshMappings();
        auto consumed=ConsumeDescriptor(ctx,bound,path);
        assert(consumed.bytes==q.data() && consumed.size==4096 && consumed.bytes[37]==0x72);
        // A never-bound slot keeps the existing descriptor fallback behavior.
        ConstantBuffer unbound{};assert(!ConsumeDescriptor(ctx,unbound,path));
        assert(descriptorDummies==before+1);
    }
    std::puts("PASS: actual full/quick/compute descriptor consumption rejects bound unmapped CBUF, idle refresh and true unbound behavior preserved");
}
void CheckBoundConstantBufferRemap() {
    using namespace skyline::gpu::interconnect;
    using namespace maxwell3d;
    InterconnectContext ctx;auto &gmmu=ctx.channelCtx.asCtx->gmmu;gmmu.blocks={{0,nullptr,{}}};
    std::vector<u8> p(4096,0x19),q(4096,0x72);
    gmmu.Map(0x1000,p.data(),p.size());
    DirtyManager manager;engine::ConstantBufferSelector selector{65536,0x1000};ConstantBuffers buffers{manager,{selector}};
    buffers.Bind(ctx,engine::ShaderStage::Vertex,0);
    auto &bound=buffers.boundConstantBuffers[0][0];
    auto retained=bound.view;
    gmmu.Map(0x1000,q.data(),q.size());
    std::array<u8,16> data{};bound.Read(ctx.executor,{data},0);
    assert(std::all_of(data.begin(),data.end(),[](auto value){return value==0x72;}));
    assert(bound.view.bytes==q.data() && bound.view.size==4096 && retained.bytes==p.data());
    auto allocations=ctx.gpu.buffer.buffers.size();
    for(size_t i{};i<1000;++i){bound.RefreshMapping();bound.Read(ctx.executor,{data},0);}
    assert(ctx.gpu.buffer.buffers.size()==allocations);
    gmmu.Map(0x1000,p.data(),p.size());buffers.RefreshMappings();
    assert(bound.view.bytes==p.data());
    gmmu.Unmap(0x1000,p.size());
    // An unrelated graphics Clear refreshes all bound CBUFs. An unmapped
    // idle binding must not abort that operation; an actual read still fails.
    buffers.RefreshMappings();assert(!bound.view);
    bool rejected{};try{bound.Read(ctx.executor,{data},0);}catch(const std::runtime_error &e){rejected=std::string(e.what())=="Bound constant buffer is not mapped";}
    assert(rejected && !bound.view);
    gmmu.Map(0x1000,q.data(),q.size());buffers.RefreshMappings();
    assert(bound.view.bytes==q.data());bound.Read(ctx.executor,{data},0);
    assert(bound.view.bytes==q.data() && bound.view.size==4096);
    // Replacing the translator is a new association even at the same numerical revision.
    auto oldTranslator=ctx.channelCtx.asCtx;
    ctx.channelCtx.asCtx=std::make_shared<InterconnectContext::As>();
    ctx.channelCtx.asCtx->gmmu.blocks={{0,nullptr,{}}};
    ctx.channelCtx.asCtx->gmmu.Map(0x1000,p.data(),p.size());bound.Read(ctx.executor,{data},0);
    assert(bound.view.bytes==p.data());
    // A remap after refresh cannot authorize a GPU consumer from this attempt.
    ctx.executor.regionOperationActive=true;ctx.executor.textureMappingEpoch={};bound.RefreshMapping();
    ctx.channelCtx.asCtx->gmmu.Map(0x1000,q.data(),q.size());
    assert(!ctx.executor.textureMappingEpoch.Acquire());
    ctx.executor.regionOperationActive=false;
    std::puts("PASS: bound CBUF CPU/descriptor refresh P->Q->P without rebind, clamp, unmapped/recovery, translator replacement, late lease rejection, stable no allocation");
}
int main() {
    if(std::getenv("CBUF_DESCRIPTOR_PROBE")){CheckBoundDescriptorConsumption();return 0;}
    CheckBoundDescriptorConsumption();
    if(std::getenv("CBUF_BOUND_MAPPING_PROBE")){CheckBoundConstantBufferRemap();return 0;}
    if(std::getenv("CBUF_MAPPING_EPOCH_PROBE")){CheckBufferMappingEpoch();return 0;}
    if(std::getenv("CBUF_CASTLE_REPLAY_REF")){ReplayCapturedCastleMacro();return 0;}
    CheckBufferMappingEpoch();
    CheckBoundConstantBufferRemap();
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
    assert(logs.empty()); // Successful constant-buffer binds do not emit errors.
    // Preserve the known upper-bound clamping behavior and explicit purge path.
    gmmu.Unmap(0x1000,4096);buffers.MarkAllDirty();
    const auto revision=gmmu.MappingRevision();const auto boundBytes=bound.bytes;const auto beforeAlloc=ctx.gpu.buffer.buffers.size();
    bool failed{};try{buffers.Bind(ctx,engine::ShaderStage::Vertex,1);}catch(const std::runtime_error &e){failed=std::string(e.what())=="Constant buffer selector is not mapped";}
    assert(failed && bound.bytes==boundBytes && ctx.gpu.buffer.buffers.size()==beforeAlloc && gmmu.MappingRevision()==revision);
    // A real interpreter Send reaches the same production Bind and must
    // propagate the unmapped-selector exception unchanged.
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
    // Retain the identity receipt regression without debug-only Inspect.
    skyline::MappingIdentity identity;const auto initial=identity.Revision();
    auto tracked=identity.Capture(10,20);
    assert(tracked.IsCurrent() && tracked.GenerationAt(20)!=0 && identity.Revision()==initial);
    {auto mutation=identity.Invalidate(20,1);}
    assert(!tracked.IsCurrent() && identity.Revision()==initial+1);
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
    std::puts("PASS: production selector/cache data, clamp, unmapped rejection, MME replay and unchanged exception");
}
