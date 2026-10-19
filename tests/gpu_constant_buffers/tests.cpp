// SPDX-License-Identifier: MPL-2.0
#include <source_location>
#include "preamble.inc"
namespace skyline { using u16=uint16_t; }
using namespace skyline;
constexpr u64 Page = constant::PageSize;
struct MM : FlatMemoryManager<u64,0,20,12,16> {
    unsigned translations{};
    auto TranslateMappings(u64 address, u64 size) {
        ++translations;
        return FlatMemoryManager::TranslateMappings(address,size);
    }
};
namespace vk {
struct BufferCopy { size_t size,srcOffset,dstOffset; };
enum class PipelineStageFlagBits { eTransfer,eAllCommands };
enum class AccessFlagBits { eTransferWrite=1,eMemoryRead=2,eMemoryWrite=4 };
inline AccessFlagBits operator|(AccessFlagBits a,AccessFlagBits b) { return AccessFlagBits(int(a)|int(b)); }
struct MemoryBarrier { AccessFlagBits srcAccessMask,dstAccessMask; };
namespace raii { struct CommandBuffer {
    void copyBuffer(int,int,BufferCopy) {}
    void pipelineBarrier(PipelineStageFlagBits,PipelineStageFlagBits,int,MemoryBarrier,int,int) {}
}; }
}
namespace skyline::dirty {
struct Handle { bool *value; };
#include "dirty.inc"
}
namespace skyline::gpu::interconnect {
struct Buffer;
inline unsigned locks{}, writes{}, reads{};
template<class T> struct ContextLock {
    ContextLock(u64,T&) { ++locks; }
    bool IsFirstUsage() { return true; }
};
template<class T> ContextLock(u64,T&)->ContextLock<T>;
struct BufferView {
    std::shared_ptr<Buffer> buffer;
    span<u8> memory;
    size_t size{};
    BufferView() = default;
    BufferView(std::shared_ptr<Buffer> b, span<u8> m): buffer{std::move(b)},memory{m},size{m.size()} {}
    explicit operator bool() const { return bool(buffer); }
    auto GetBuffer() const { return buffer; }
    size_t GetOffset() const { return 0; }
    struct Binding { int buffer{}; size_t offset{}; };
    Binding GetBinding(auto&) const { return {}; }
    bool Write(span<u8> input,u32 offset,auto&,auto&&... callback) {
        ++writes;
        if (!*this || offset>size || input.size()>size-offset) throw std::runtime_error("invalid write reached transport");
        std::copy(input.begin(),input.end(),memory.begin()+offset);
        return false;
    }
    void Read(bool,auto&&,span<u8> output,size_t offset) {
        ++reads;
        if (!*this || offset>memory.size() || output.size()>memory.size()-offset) throw std::runtime_error("invalid read reached transport");
        std::copy_n(memory.begin()+offset,output.size(),output.begin());
    }
};
struct Buffer : std::enable_shared_from_this<Buffer> {
    span<u8> memory;
    std::vector<u8> owned;
    BufferView TryGetView(span<u8> requested) {
        return memory.contains(requested) ? BufferView{shared_from_this(),requested} : BufferView{};
    }
    void BlockAllCpuBackingWrites() {}
};
struct BufferManager {
    unsigned preparations{};
    void PrepareBufferAccess(Buffer&,bool=false,u64=0,u64=std::numeric_limits<u64>::max()) { ++preparations; }
    BufferView FindOrCreate(span<u8> memory,u64,auto&&) {
        auto buffer=std::make_shared<Buffer>(); buffer->memory=memory;
        return {buffer,memory};
    }
    BufferView FindOrCreateDisjoint(const std::vector<span<u8>> &mappings,size_t offset,size_t size,u64,auto&&) {
        auto buffer=std::make_shared<Buffer>();
        size_t total{}; for (auto mapping:mappings) total+=mapping.size();
        buffer->owned.resize(total);
        size_t cursor{};
        for (auto mapping:mappings) {
            if (mapping.valid()) std::copy(mapping.begin(),mapping.end(),buffer->owned.begin()+cursor);
            cursor+=mapping.size();
        }
        buffer->memory=span<u8>{buffer->owned};
        return {buffer,buffer->memory.subspan(offset,size)};
    }
};
struct FenceCycle {};
struct GPU {
    BufferManager buffer;
    struct Mega { struct Allocation { size_t offset{}; int buffer{}; }; Allocation Push(auto&,auto&) { return {}; } } megaBufferAllocator;
};
struct CommandExecutor {
    u64 tag{1}; int usageTracker{},cycle{};
    void AttachLockedBuffer(std::shared_ptr<Buffer>,ContextLock<Buffer>&&) {}
    void AttachLockedBufferView(BufferView,ContextLock<BufferView>&&) {}
    void AddCheckpoint(const char*) {}
    void AddOutsideRpCommand(auto&&) {}
};
struct InterconnectContext {
    struct AddressSpace { MM gmmu; } addressSpace;
    struct Channel { AddressSpace *asCtx; } channelCtx{&addressSpace};
    GPU gpu;
    CommandExecutor executor;
};
struct DirtyManager { void Bind(dirty::Handle,auto&) {} };
namespace engine {
#include "engine_types.inc"
enum class ShaderStage { Vertex };
constexpr size_t ShaderStageCount=1,ShaderStageConstantBufferCount=4;
}
#define LOGW(...) [](const auto &...) {}(__VA_ARGS__)
#include "cache.inc"
#include "read.inc"
}
#include "selector.inc"
using namespace skyline::gpu::interconnect;
using namespace skyline::gpu::interconnect::maxwell3d;
struct Fixture {
    InterconnectContext ctx;
    DirtyManager manager;
    engine::ConstantBufferSelector selector{.size=0x10000, ._pad_=0, .address={.high=0, .low=Page}};
    ConstantBufferSelectorState state{dirty::Handle{nullptr},manager,{selector}};
    ConstantBuffers buffers{manager,{selector}};
    std::vector<u8> a=std::vector<u8>(Page*4,0x21), b=std::vector<u8>(Page*4,0x62);
    void Map(size_t size=Page*4) { ctx.addressSpace.gmmu.Map(Page,a.data(),size); }
    BufferView &Bound() { return buffers.boundConstantBuffers[0][0].view; }
    void Bind() { buffers.Bind(ctx,engine::ShaderStage::Vertex,0); }
};
void Require(bool value,const char *message) { if (!value) throw std::runtime_error(message); }
template<class F> void RejectsBeforeAccess(F operation,const char *message) {
    const auto oldLocks=locks,oldWrites=writes,oldReads=reads;
    bool rejected{}; try { operation(); } catch (const std::runtime_error&) { rejected=true; }
    Require(rejected,message);
    Require(locks==oldLocks && writes==oldWrites && reads==oldReads,"invalid access reached lock/transport");
}
void CheckHintAndBind() {
    Fixture f; f.Map(); f.Bind();
    Require(bool(f.Bound()) && f.Bound().size==Page*4,"64 KiB hint rejected a 16 KiB mapped constant buffer");
    Require(!f.buffers.quickBindEnabled,"Bind changed existing DisableQuickBind behavior");
    f.state.Flush(f.ctx,16); Require(f.state.view->size==Page*4,"Flush failed mapped prefix");
    const auto translations=f.ctx.addressSpace.gmmu.translations,preparations=f.ctx.gpu.buffer.preparations;
    f.state.Refresh(f.ctx,32);
    Require(f.ctx.addressSpace.gmmu.translations==translations,"unchanged selector rescanned capacity");
    Require(f.ctx.gpu.buffer.preparations==preparations+1,"unchanged selector skipped alias access preflight");
    f.selector.address.low=Page*2;
    f.state.Refresh(f.ctx); Require(f.state.view->size==Page*3,"selector address change retained old prefix");
    f.selector.size=Page;
    f.state.Refresh(f.ctx); Require(f.state.view->size==Page,"selector capacity change retained old size");
    const auto beforePurge=f.ctx.addressSpace.gmmu.translations;
    f.state.PurgeCaches(); f.state.Refresh(f.ctx);
    Require(f.state.view->size==Page && f.ctx.addressSpace.gmmu.translations>beforePurge,"purged selector reused old capacity cache");
}
void CheckHoleAndDisjoint() {
    Fixture f; f.Map(Page); f.ctx.addressSpace.gmmu.Map(Page*3,f.b.data(),Page);
    f.state.Flush(f.ctx); Require(f.state.view->size==Page,"selector crossed a true unmapped hole");
    f.state.Refresh(f.ctx,Page+1); Require(!*f.state.view,"selector accepted required bytes through hole");
    f.ctx.addressSpace.gmmu.Map(Page*2,f.b.data(),Page);
    f.state.Refresh(f.ctx,Page*2); Require(f.state.view->size==Page*3,"selector lost disjoint resident pages");
    Require(f.state.view->memory[0]==0x21 && f.state.view->memory[Page]==0x62,"disjoint prefix lost physical data");
    // Two disjoint mapped pages followed by an unmapped tail is the bind regression.
    f.ctx.addressSpace.gmmu.Unmap(Page*3,Page); f.Bind(); Require(f.Bound().size==Page*2,"disjoint prefix bind rejected");
}
void CheckSparse() {
    Fixture f; f.ctx.addressSpace.gmmu.MapSparse(Page,Page*2); f.Bind();
    Require(f.Bound().size==Page*2,"explicit sparse mapping rejected");
    Require(std::all_of(f.Bound().memory.begin(),f.Bound().memory.end(),[](u8 b){return b==0;}),"sparse prefix not zero");
    f.state.Flush(f.ctx,Page*2+1); Require(!*f.state.view,"sparse prefix hid required unmapped tail");
}
void CheckRemapping() {
    Fixture f; f.Map(); f.Bind();
    InterconnectContext::AddressSpace other;
    other.gmmu.Map(Page,f.b.data(),Page);
    Require(other.gmmu.GetMappingGeneration()==f.ctx.addressSpace.gmmu.GetMappingGeneration(),"test needs equal generations");
    f.ctx.channelCtx.asCtx=&other; f.Bind();
    Require(f.Bound().size==Page && f.Bound().memory[0]==0x62,"equal-generation address space switch retained stale mapping");
    f.ctx.channelCtx.asCtx=&f.ctx.addressSpace; f.Bind();
    Require(f.Bound().size==Page*4 && f.Bound().memory[0]==0x21,"return to previous address space retained stale mapping");
    f.ctx.addressSpace.gmmu.Unmap(Page,Page*4); f.ctx.addressSpace.gmmu.Map(Page,f.b.data(),Page); f.Bind();
    Require(f.Bound().size==Page && f.Bound().memory[0]==0x62,"same selector retained stale larger mapping");
    f.ctx.addressSpace.gmmu.Map(Page,f.b.data(),Page*4); f.Bind();
    Require(f.Bound().size==Page*4,"same selector did not grow after remap");
    other.gmmu.Map(Page,f.a.data(),Page);
    f.ctx.channelCtx.asCtx=&other; f.Bind();
    Require(f.Bound().size==Page && f.Bound().memory[0]==0x21,"address space switch retained stale mapping");
    other.gmmu.Unmap(Page,Page);
    RejectsBeforeAccess([&]{f.Bind();},"fully unmapped selector bound");
}
void CheckLoad() {
    Fixture f; f.Map(Page); f.selector.size=4;
    std::array<u32,2> input{0x12345678,0x87654321};
    f.buffers.Load(f.ctx,span<u32>{input},32);
    Require(std::memcmp(f.a.data()+32,input.data(),sizeof(input))==0,"Load omitted offset from required extent");
    f.selector.size=0x10000; f.buffers.MarkAllDirty();
    RejectsBeforeAccess([&]{f.buffers.Load(f.ctx,span<u32>{input},Page-4);},"Load accepted bytes beyond mapped prefix");
    RejectsBeforeAccess([&]{f.buffers.Load(f.ctx,span<u32>{input},std::numeric_limits<u32>::max());},"Load accepted overflowing actual range");
    f.ctx.addressSpace.gmmu.Unmap(Page,Page);
    RejectsBeforeAccess([&]{f.buffers.Load(f.ctx,span<u32>{input},0);},"unmapped Load reached a lock");
}
void CheckStrictCacheAndRead() {
    Fixture f; f.Map(Page); CachedMappedBufferView strict;
    strict.Update(f.ctx,Page,Page*2); Require(!*strict,"general buffers regained prefix truncation");
    auto backing=std::make_shared<Buffer>(); backing->memory=span<u8>{f.a};
    ConstantBuffer cb{{backing,backing->memory}}; cb.view.size=16;
    std::array<u8,4> output{};
    cb.Read(f.ctx.executor,span<u8>{output},16-output.size());
    Require(output[0]==0x21,"valid end-of-prefix read failed");
    RejectsBeforeAccess([&]{cb.Read(f.ctx.executor,span<u8>{output},16);},"read crossed clipped selector prefix");
    RejectsBeforeAccess([&]{cb.Read(f.ctx.executor,span<u8>{output},std::numeric_limits<size_t>::max());},"read offset overflow accepted");
}
int main(int argc,char**argv) {
    const std::string selection=argc>1?argv[1]:"all";
    const std::pair<const char*,void(*)()> tests[]={{"hint",CheckHintAndBind},{"holes",CheckHoleAndDisjoint},{"sparse",CheckSparse},{"remap",CheckRemapping},{"load",CheckLoad},{"read",CheckStrictCacheAndRead}};
    for(auto [name,test]:tests) if(selection=="all" || selection==name) { test(); std::cout<<"PASS: "<<name<<'\n'; }
}
