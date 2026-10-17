// SPDX-License-Identifier: MPL-2.0
// Vulkan, traps and fence transport are facades; alias decisions and memory copies
// below are compiled from the production buffer and manager method bodies.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
using u8=uint8_t; using u32=uint32_t;
struct FenceCycle {};
class BufferDelegate;
namespace vk { using DeviceSize=uint64_t; }
constexpr vk::DeviceSize VK_WHOLE_SIZE=~vk::DeviceSize{};
namespace constant { constexpr size_t PageSize=4096; }
using ContextTag=uint64_t;
struct exception:std::runtime_error { using std::runtime_error::runtime_error; };
template<class T> struct span:std::span<T> {
    using std::span<T>::span;
    span(std::span<T> value):std::span<T>(value){}
    span(std::nullptr_t,size_t count):std::span<T>(static_cast<T*>(nullptr),count){}
    bool valid() const { return this->data()!=nullptr; }
};
using GuestBuffer=span<u8>;
template<class T> struct IntervalList {
    std::vector<GuestBuffer> intervals;
    void Insert(GuestBuffer value) { intervals.push_back(value); }
    void Clear(){intervals.clear();}
    void Merge(const IntervalList &other){intervals.insert(intervals.end(),other.intervals.begin(),other.intervals.end());}
    bool Intersect(GuestBuffer value){for(auto interval:intervals){auto a=reinterpret_cast<uintptr_t>(value.data()),b=reinterpret_cast<uintptr_t>(interval.data());if(a<=b?b-a<value.size():a-b<interval.size())return true;}return false;}
};
struct Buffer;
struct BufferManager;
struct BufferView { Buffer *buffer{}; vk::DeviceSize offset{},size{}; explicit operator bool() const{return buffer;} };
struct UsageTracker { IntervalList<u8*> dirtyIntervals,sequencedIntervals; };
struct Backing { std::vector<u8> bytes; explicit Backing(size_t size):bytes(size){} u8 *data(){return bytes.data();} };
struct GPU {
    struct State { struct Settings { bool direct{}; bool *useDirectMemoryImport{&direct}; }; std::shared_ptr<Settings> settings=std::make_shared<Settings>(); struct Process { struct Trap { void TrapRegions(int,bool){} } trap; }; std::shared_ptr<Process> process=std::make_shared<Process>(); } state;
    BufferManager &buffer;
};
constexpr size_t Page=constant::PageSize;
struct PhysicalMemory {
    int fd=static_cast<int>(syscall(SYS_memfd_create,"buffer-alias-tests",0));
    u8 *data{}; size_t size=Page*16;
    PhysicalMemory(){assert(fd>=0 && ftruncate(fd,size)==0);data=static_cast<u8*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));assert(data!=MAP_FAILED);}
    ~PhysicalMemory(){munmap(data,size);close(fd);}
    span<u8> Mirror(const std::vector<GuestBuffer> &mappings) {
        size_t total{}; for(auto m:mappings) total+=m.size();
        auto ptr=static_cast<u8*>(mmap(nullptr,total,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(ptr!=MAP_FAILED);
        size_t offset{};
        for(auto m:mappings) {
            auto result=mmap(ptr+offset,m.size(),PROT_READ|PROT_WRITE,
                MAP_FIXED|(m.valid()?MAP_SHARED:MAP_PRIVATE|MAP_ANONYMOUS),m.valid()?fd:-1,m.valid()?m.data()-data:0);
            assert(result==ptr+offset); offset+=m.size();
        }
        return {ptr,total};
    }
} physical;
struct Buffer:std::enable_shared_from_this<Buffer> {
    GPU &gpu; std::optional<GuestBuffer> guest; std::vector<GuestBuffer> guestMappings;
    std::atomic<bool> aliasValid{true};
    bool everHadInlineUpdate{};
    bool isAggregate{},isDirect{},directGpuWritesActive{},currentExecutionGpuDirty{};
    enum class DirtyState { Clean,CpuDirty,GpuDirty } dirtyState{DirtyState::CpuDirty};
    enum class BackingImmutability { None,SequencedWrites,AllWrites } backingImmutability{};
    IntervalList<u8*> aliasPendingWrites;
    std::recursive_mutex mutex,stateMutex; std::atomic<ContextTag> tag{};
    span<u8> mirror; std::optional<Backing> backing; std::optional<int> trapHandle;
    std::shared_ptr<FenceCycle> cycle;
    BufferDelegate *delegate{};
    bool directTrackedShadowActive{}; std::vector<u8> directTrackedShadow; IntervalList<size_t> directTrackedWrites;
    void EnableTrackedShadowDirect(){directTrackedShadowActive=true;directTrackedShadow.resize(mirror.size());}
    std::function<void()> fenceCompletion;
    unsigned waits{},hostSyncs{},guestSyncs{},sequence{},writeGeneration{};
    Buffer(int&,GPU &gpu,const std::vector<GuestBuffer>&mappings,size_t):gpu(gpu),guestMappings(mappings),isAggregate(true),mirror(physical.Mirror(mappings)),backing(mirror.size()){guest=mirror;}
    Buffer(int&,GPU &,GuestBuffer,size_t,bool);
    ~Buffer(){munmap(mirror.data(),mirror.size());}
    bool LockWithTag(ContextTag value){if(value && value==tag)return false;mutex.lock();tag=value;return true;}
    void unlock();
    void AllowAllBackingWrites(){backingImmutability=BackingImmutability::None;}
    bool SequencedCpuBackingWritesBlocked(){return backingImmutability!=BackingImmutability::None;}
    bool AllCpuBackingWritesBlocked(){return backingImmutability==BackingImmutability::AllWrites;}
    void AdvanceSequence(){++sequence;}
    void WaitOnFence(){++waits;if(cycle && fenceCompletion)fenceCompletion();cycle={};}
    bool PollFence(){return true;}
    bool RefreshGpuWritesActiveDirect(bool,const std::function<void()> & = {}){directGpuWritesActive=false;return false;}
    void SetupStagedTraps(){}
    BufferView GetView(vk::DeviceSize offset,vk::DeviceSize size){return {this,offset,size};}
    void MarkGpuDirtyImpl(){dirtyState=DirtyState::GpuDirty;backingImmutability=BackingImmutability::AllWrites;}
    void VisitPhysicalRanges(vk::DeviceSize,vk::DeviceSize,const std::function<void(GuestBuffer)>&)const;
    void TrackGuestUsage(IntervalList<u8*>&,vk::DeviceSize,vk::DeviceSize);
    void ClearSparseHoles(bool=false);
    bool HasSparseHoles(vk::DeviceSize,vk::DeviceSize)const;
    void SynchronizeHost(bool=false);
    bool SynchronizeGuest(bool=false,bool=false);
    void MarkGpuDirty(UsageTracker&);
    bool Write(span<u8>,vk::DeviceSize,UsageTracker&,const std::function<void()>& callback = {});
    bool WriteImplStaged(span<u8>,vk::DeviceSize,const std::function<void()>&);
    bool WriteImplDirect(span<u8>,vk::DeviceSize,UsageTracker&,const std::function<void()>&){throw exception("Unexpected direct transport call");}
    void CopyFrom(vk::DeviceSize,Buffer*,vk::DeviceSize,vk::DeviceSize,UsageTracker&,const std::function<void()>&);
    void CopyFromImplDirect(vk::DeviceSize,Buffer*,vk::DeviceSize,vk::DeviceSize,UsageTracker&,const std::function<void()>&){throw exception("Mixed copy entered direct-only path");}
    void CopyFromImplStaged(vk::DeviceSize,Buffer*,vk::DeviceSize,vk::DeviceSize,UsageTracker&,const std::function<void()>&){throw exception("Mixed copy entered staged-only path");}
};
template<class T> struct ContextLock {
    T &object; bool first;
    ContextLock(ContextTag tag,T &object):object(object),first(object.LockWithTag(tag)){}
    ContextLock(ContextLock &&other):object(other.object),first(other.first){other.first=false;}
    ~ContextLock(){if(first)object.unlock();}
    bool IsFirstUsage()const{return first;}
    bool OwnsLock()const{return first;}
    void Release(){first=false;}
};
struct BufferMappingReconciliationRequest { BufferManager *manager; std::shared_ptr<Buffer>target;bool write;vk::DeviceSize offset,size;void Complete()const; };
struct BufferManager {
    GPU gpu{.state={},.buffer=*this}; int delegateAllocatorState{};size_t nextBufferId{};
    std::vector<std::shared_ptr<Buffer>> aggregateBuffers;std::vector<std::weak_ptr<Buffer>>aliasBuffers;std::atomic<bool>hasAggregates{};std::mutex aliasMutex;
    std::recursive_mutex recreationMutex;
    struct LockedBuffer {
        std::shared_ptr<Buffer>buffer;ContextLock<Buffer>lock;std::unique_lock<std::recursive_mutex>stateLock;
        LockedBuffer(std::shared_ptr<Buffer> value,ContextTag tag):buffer(value),lock(tag,*value),stateLock(value->stateMutex){}
        Buffer*operator->()const{return buffer.get();}std::shared_ptr<Buffer>&operator*(){return buffer;}
    };
    using LockedBuffers=std::vector<LockedBuffer>;
    LockedBuffer CoalesceBuffers(span<u8>,const LockedBuffers&,ContextTag);
    std::vector<std::shared_ptr<Buffer>>SnapshotAliases();
    void PrepareBufferAccess(Buffer&,bool=false,vk::DeviceSize=0,vk::DeviceSize=VK_WHOLE_SIZE);
    void NotifyBufferWrite(Buffer&,vk::DeviceSize,vk::DeviceSize);
    BufferView FindOrCreateDisjoint(const std::vector<GuestBuffer>&,vk::DeviceSize,vk::DeviceSize,ContextTag,const std::function<void(std::shared_ptr<Buffer>,ContextLock<Buffer>&&)>&);
};
#define TRACE_EVENT(...)
#define LOGE(...) ((void)0)
#include "delegates.inc"
Buffer::Buffer(int &allocator,GPU &gpu,GuestBuffer mapping,size_t id,bool direct):Buffer(allocator,gpu,std::vector<GuestBuffer>{mapping},id){isAggregate=false;isDirect=direct;guest=mapping;delegate=new BufferDelegate(this);}
#include "authority.inc"
std::shared_ptr<Buffer> Make(BufferManager &manager,std::vector<GuestBuffer> mappings,bool aggregate=true) {
    auto buffer=std::make_shared<Buffer>(manager.delegateAllocatorState,manager.gpu,mappings,0);
    buffer->isAggregate=aggregate; if(!aggregate)buffer->guest=mappings.front(); buffer->delegate=new BufferDelegate(buffer.get()); manager.aliasBuffers.push_back(buffer);manager.hasAggregates=true;return buffer;
}
void Fill(span<u8> bytes,u8 value){std::fill(bytes.begin(),bytes.end(),value);}
#ifndef GPU_DISJOINT_BOUNDARY_TEST
int main(){
    auto a=GuestBuffer{physical.data,Page},b=GuestBuffer{physical.data+Page*2,Page},c=GuestBuffer{physical.data+Page*4,Page};
    Fill(a,0x11);Fill(b,0x22);Fill(c,0x33);
    BufferManager manager;
    auto canonicalA=Make(manager,{a},false),canonicalB=Make(manager,{b},false),unrelated=Make(manager,{c},false);
    auto aggregate=Make(manager,{a,b});
    canonicalA->dirtyState=Buffer::DirtyState::GpuDirty;Fill(canonicalA->backing->bytes,0xA5);
    unrelated->dirtyState=Buffer::DirtyState::GpuDirty;Fill(unrelated->backing->bytes,0xC5);
    manager.PrepareBufferAccess(*aggregate,false,0,Page);
    aggregate->SynchronizeHost();
    assert(a[0]==0xA5 && b[0]==0x22 && c[0]==0x33);
    assert(aggregate->backing->bytes[0]==0xA5 && aggregate->backing->bytes[Page]==0x22);
    assert(unrelated->waits==0 && unrelated->dirtyState==Buffer::DirtyState::GpuDirty);
    const auto waits=canonicalA->waits;
    manager.PrepareBufferAccess(*aggregate,false,0,Page);
    assert(canonicalA->waits==waits); // Clean aliases do not force copies or waits.
    aggregate->dirtyState=Buffer::DirtyState::GpuDirty;Fill(aggregate->backing->bytes,0x5A);
    manager.PrepareBufferAccess(*canonicalB);
    assert(a[0]==0x5A && b[0]==0x5A && canonicalB->dirtyState==Buffer::DirtyState::CpuDirty);
    manager.NotifyBufferWrite(*aggregate,Page,Page);
    aggregate->backingImmutability=Buffer::BackingImmutability::AllWrites;aggregate->tag=7;
    bool deferred{};
    try{manager.PrepareBufferAccess(*canonicalB);}catch(const BufferMappingReconciliationRequest &request){deferred=true;assert(request.target==canonicalB);}
    assert(deferred && aggregate->tag==7 && aggregate->backingImmutability==Buffer::BackingImmutability::AllWrites);
    manager.PrepareBufferAccess(*canonicalA); // B-only queued writes cannot stall reads of A.
    aggregate->backingImmutability=Buffer::BackingImmutability::None;aggregate->tag=0;
    manager.PrepareBufferAccess(*canonicalB);
    canonicalA->tag=9;canonicalA->backingImmutability=Buffer::BackingImmutability::SequencedWrites;
    manager.PrepareBufferAccess(*aggregate,true,Page,Page); // Unrelated read-only A cannot block writes to B.
    deferred=false;try{manager.PrepareBufferAccess(*aggregate,true,0,Page);}catch(const BufferMappingReconciliationRequest&){deferred=true;}
    assert(deferred);canonicalA->tag=0;canonicalA->backingImmutability=Buffer::BackingImmutability::None;
    IntervalList<u8*> intervals;auto sparse=Make(manager,{a,{nullptr,Page},b});
    sparse->TrackGuestUsage(intervals,Page-7,Page+14);
    assert(intervals.intervals.size()==2 && intervals.intervals[0].size()==7 && intervals.intervals[1].size()==7);
    Fill(sparse->mirror,0xFF);sparse->ClearSparseHoles();assert(sparse->mirror[Page]==0 && a[0]==0xFF && b[0]==0xFF);
    UsageTracker usage;bool rejected{};
    try{sparse->MarkGpuDirty(usage);}catch(const exception&){rejected=true;}
    assert(rejected); // A staged backing cannot discard writes to holes within a dispatch.
    std::vector<u8> input(Page*3,0xD3);
    sparse->dirtyState=Buffer::DirtyState::Clean;
    assert(!sparse->Write(input,0,usage));
    assert(a[0]==0xD3 && b[0]==0xD3 && sparse->mirror[Page]==0 && sparse->backing->bytes[Page]==0);
    sparse->backingImmutability=Buffer::BackingImmutability::SequencedWrites;rejected=false;
    try{sparse->Write(input,0,usage);}catch(const exception&){rejected=true;}
    assert(rejected);sparse->backingImmutability=Buffer::BackingImmutability::None;
    auto repeated=Make(manager,{a,a});rejected=false;
    try{manager.PrepareBufferAccess(*repeated,true);}catch(const exception&){rejected=true;}
    assert(rejected);
    auto staged=Make(manager,{{physical.data+Page*6,Page},{physical.data+Page*8,Page}});
    auto direct=Make(manager,{{physical.data+Page*10,Page*2}},false);direct->isDirect=true;
    staged->dirtyState=Buffer::DirtyState::GpuDirty;Fill(staged->backing->bytes,0xB7);
    unsigned copies{};
    direct->CopyFrom(0,staged.get(),0,Page*2,usage,[&]{++copies;std::memcpy(direct->mirror.data(),staged->backing->data(),Page*2);});
    assert(copies==1 && direct->mirror[0]==0xB7 && direct->mirror[Page]==0xB7);
    auto pendingDirect=Make(manager,{{physical.data+Page*12,Page}},false);pendingDirect->isDirect=true;
    auto pendingAlias=Make(manager,{{physical.data+Page*12,Page},{physical.data+Page*14,Page}});
    pendingDirect->LockWithTag(12);
    manager.NotifyBufferWrite(*pendingDirect,0,Page);
    pendingDirect->cycle=std::make_shared<FenceCycle>();pendingDirect->fenceCompletion=[&]{Fill(pendingDirect->mirror,0xE5);};
    pendingDirect->unlock();
    manager.PrepareBufferAccess(*pendingAlias,false,0,Page);pendingAlias->SynchronizeHost();
    assert(pendingDirect->waits==1 && pendingAlias->backing->bytes[0]==0xE5); // Submitted direct shadow writes remain pending across unlock.
    pendingDirect->cycle=std::make_shared<FenceCycle>();const auto readWaits=pendingDirect->waits;
    manager.PrepareBufferAccess(*pendingAlias,true,0,Page);
    assert(pendingDirect->waits==readWaits+1); // CPU writes cannot race an imported alias's prior GPU readers.
    BufferDelegate oldDelegate{canonicalA.get()},middleDelegate{aggregate.get()},newDelegate{staged.get()};
    oldDelegate.Link(&middleDelegate,Page);middleDelegate.Link(&newDelegate,Page*2);
    assert(oldDelegate.GetBuffer()==staged.get() && oldDelegate.GetOffset()==Page*3);
    auto first=manager.FindOrCreateDisjoint({a,b},0,Page*2,0,{});
    auto remapped=manager.FindOrCreateDisjoint({a,c},0,Page*2,0,{});
    assert(first.size==Page*2 && remapped.size==Page*2 && first.buffer!=remapped.buffer);
    {
        BufferManager coalescing;
        auto old=Make(coalescing,{a},false),queued=Make(coalescing,{a,c});
        old->LockWithTag(44);old->backingImmutability=Buffer::BackingImmutability::SequencedWrites;old->dirtyState=Buffer::DirtyState::Clean;
        queued->LockWithTag(44);queued->dirtyState=Buffer::DirtyState::Clean;queued->backingImmutability=Buffer::BackingImmutability::AllWrites;
        coalescing.NotifyBufferWrite(*queued,Page,Page);
        BufferManager::LockedBuffers sources;sources.emplace_back(old,44);
        bool requested{};std::weak_ptr<Buffer> abandonedReplacement;
        try{auto result=coalescing.CoalesceBuffers({a.data(),Page*5},sources,44);coalescing.PrepareBufferAccess(**result);}
        catch(const BufferMappingReconciliationRequest &request){
            requested=true;abandonedReplacement=request.target;
            assert(request.target.get()!=old.get() && request.target->mirror.size()==Page*5);
            assert(old->delegate->GetBuffer()==old.get());
            // Model executor submission and reconciliation while the request
            // alone owns the replacement that was never inserted in the manager.
            queued->AllowAllBackingWrites();request.Complete();
        }
        assert(requested && abandonedReplacement.expired());
        assert(old->delegate->GetBuffer()==old.get()); // Must remain valid after the handler destroys its request.
        coalescing.PrepareBufferAccess(*old);
        {auto replacement=coalescing.CoalesceBuffers({a.data(),Page*5},sources,44);assert(old->delegate->GetBuffer()==replacement.buffer.get());}
        old->unlock();queued->unlock();
    }
    std::cout<<"GPU disjoint buffer authority tests passed\n";
}

#endif
