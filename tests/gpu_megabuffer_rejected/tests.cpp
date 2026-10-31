// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cassert>
#include <bitset>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <vector>
#include "ssbo_diagnostics.h"
namespace vk { using DeviceSize=uint64_t; }
namespace skyline::gpu {
using u8=uint8_t; using ContextTag=uint64_t;
struct FenceCycle {};
struct BufferBinding {
    uint64_t buffer{}, offset{}, size{};
    explicit operator bool()const{return buffer!=0;}
};
constexpr uint64_t MegaBufferChunkSize{25*1024*1024};
class MegaBufferAllocator {
    uint64_t next{1};
public:
    struct Allocation {
        uint64_t buffer{},offset{};
        std::span<u8> region{};
        explicit operator bool()const{return offset!=0;}
    };
    std::map<uint64_t,std::vector<u8>> storage;
    uint64_t copies{},bytes{};
    Allocation Push(const std::shared_ptr<FenceCycle>&,std::span<u8> data,bool) {
        auto id{next++};auto &v{storage[id]};v.assign(data.begin(),data.end());
        ++copies;bytes+=data.size();return {id,1,v};
    }
    std::span<const u8> Read(BufferBinding b)const {
        return std::span<const u8>{storage.at(b.buffer)}.subspan(b.offset-1,b.size);
    }
};
class Buffer {
public:
    // Only engine/Vulkan dependencies are replaced. These fields model the
    // production state read/written by the extracted method bodies.
    enum class DirtyState{Clean,CpuDirty,GpuDirty};
    static constexpr uint64_t MegaBufferingDisableThreshold{256*1024};
    static constexpr uint32_t FrequentlySyncedThreshold{6};
    uint64_t id{},sequenceNumber{6},lastExecutionTag{},megaBufferViewAccumulatedSize{};
    bool isDirect{},everHadInlineUpdate{},directTrackedShadowActive{},gpuWritesActive{},megaBufferTableUsed{},unifiedMegaBufferEnabled{};
    DirtyState dirtyState{DirtyState::Clean};
    int megaBufferTableShift;
    std::vector<u8> bytes;
    std::span<u8> mirror;
    std::vector<MegaBufferAllocator::Allocation> megaBufferTable;
    std::bitset<0x500> megaBufferTableValidity{};
    MegaBufferAllocator::Allocation unifiedMegaBuffer{};
    explicit Buffer(size_t size):megaBufferTableShift(static_cast<int>(std::max<size_t>(std::bit_width(size/0x500-1),8))),bytes(size),mirror(bytes),megaBufferTable(size/(size_t{1}<<megaBufferTableShift)) {
        for(size_t i{};i<size;++i)bytes[i]=static_cast<u8>((i*17+13)%251);
    }
    bool RefreshGpuWritesActiveDirect(){return gpuWritesActive;}
    void ResetMegabufferState();
    void AdvanceSequence();
    bool ValidateMegaBufferViewImplDirect(vk::DeviceSize);
    bool ValidateMegaBufferViewImplStaged(vk::DeviceSize);
    bool ValidateMegaBufferView(vk::DeviceSize);
    BufferBinding TryMegaBufferView(const std::shared_ptr<FenceCycle>&,MegaBufferAllocator&,ContextTag,vk::DeviceSize,vk::DeviceSize);
};
}
#include "production.inc"
using namespace skyline::gpu;
void CheckBytes(const Buffer &b,const MegaBufferAllocator &a,BufferBinding binding,size_t offset,size_t size) {
    assert(binding && binding.size==size);
    auto data{a.Read(binding)};assert(data.size()==size);
    assert(std::equal(data.begin(),data.end(),b.bytes.begin()+offset));
}
int main(int argc,char**) {
    const auto cycle{std::make_shared<FenceCycle>()};
    if(argc==1) {
        Buffer b{8470528};MegaBufferAllocator a;
        assert(!b.TryMegaBufferView(cycle,a,1,73728,8388608));
        auto small{b.TryMegaBufferView(cycle,a,1,0,4)};
        CheckBytes(b,a,small,0,4);
        std::cout << "rejected 8MiB then 4B: actually copied=" << a.bytes << " expected=4\n" << std::flush;
        assert(a.bytes==4); // Fails on the unmodified parent: copies 8,470,528.
        for(unsigned i{};i<10;++i) assert(!b.TryMegaBufferView(cycle,a,1,73728,8388608));
        assert(a.bytes==4); // Rejected requests never become copied-byte votes.
    }
    {
        // Physical size and small-view offsets observed in buffer 172. The
        // production constructor selects 8 KiB table entries for this size.
        Buffer b{8470528};b.megaBufferTableShift=13;MegaBufferAllocator a;
        assert(!b.TryMegaBufferView(cycle,a,1,73728,8388608));
        auto x{b.TryMegaBufferView(cycle,a,1,768,4)};CheckBytes(b,a,x,768,4);
        assert(a.bytes==772);
        auto y{b.TryMegaBufferView(cycle,a,1,1024,4)};CheckBytes(b,a,y,1024,4);
        assert(a.bytes==1800); // 772 + 1028, not two whole-buffer snapshots.
        assert(!b.TryMegaBufferView(cycle,a,2,73728,8388608));
        auto z{b.TryMegaBufferView(cycle,a,2,768,4)};CheckBytes(b,a,z,768,4);
        assert(a.bytes==2828 && !b.unifiedMegaBufferEnabled); // Retains the previous 1028-byte capacity.
    }
    {
        Buffer b{4096};MegaBufferAllocator a;
        auto first{b.TryMegaBufferView(cycle,a,1,0,768)};CheckBytes(b,a,first,0,768);
        auto second{b.TryMegaBufferView(cycle,a,1,1024,768)};CheckBytes(b,a,second,1024,768);
        auto third{b.TryMegaBufferView(cycle,a,1,2048,768)};CheckBytes(b,a,third,2048,768);
        // Genuine copies > half the mirror retain normal full-copy promotion.
        auto full{b.TryMegaBufferView(cycle,a,1,3072,16)};CheckBytes(b,a,full,3072,16);
        assert(a.copies==4 && a.bytes==6400 && b.unifiedMegaBufferEnabled);
        auto reused{b.TryMegaBufferView(cycle,a,1,16,16)};CheckBytes(b,a,reused,16,16);assert(a.copies==4);
        const auto old{a.Read(full)[0]};b.bytes[3072]^=0x80;
        auto fresh{b.TryMegaBufferView(cycle,a,2,3072,16)};CheckBytes(b,a,fresh,3072,16);
        assert(a.copies==5 && a.bytes==10496 && a.Read(full)[0]==old);
        const auto previous{a.Read(fresh)[0]};b.bytes[3072]^=0x40;b.AdvanceSequence();
        auto sequenced{b.TryMegaBufferView(cycle,a,2,3072,16)};CheckBytes(b,a,sequenced,3072,16);
        assert(a.copies==6 && a.bytes==14592 && a.Read(fresh)[0]==previous);
    }
    {
        // Genuine smaller copies must still enable the original unified path,
        // including a later oversized view when that full snapshot already exists.
        Buffer b{1048576};MegaBufferAllocator a;
        for(size_t offset:{0U,262144U,524288U})CheckBytes(b,a,b.TryMegaBufferView(cycle,a,1,offset,204800),offset,204800);
        CheckBytes(b,a,b.TryMegaBufferView(cycle,a,1,800000,4),800000,4);
        assert(b.unifiedMegaBufferEnabled && a.bytes==1662976);
        CheckBytes(b,a,b.TryMegaBufferView(cycle,a,1,0,524288),0,524288);
        assert(a.bytes==1662976);
    }
    {
        // Marvel's observed 192 KiB inferred read is below the unchanged policy
        // threshold, so normal chunking/full-copy promotion remains available.
        Buffer b{200704};MegaBufferAllocator a;
        CheckBytes(b,a,b.TryMegaBufferView(cycle,a,1,0,196608),0,196608);
        assert(a.bytes==196608);
        CheckBytes(b,a,b.TryMegaBufferView(cycle,a,1,0,196608),0,196608);
        assert(b.unifiedMegaBufferEnabled && a.bytes==397312);
    }
    for(bool direct:{false,true}) {
        Buffer b{4096};MegaBufferAllocator a;b.isDirect=direct;
        if(direct){b.everHadInlineUpdate=true;b.gpuWritesActive=true;}
        else b.dirtyState=Buffer::DirtyState::GpuDirty;
        assert(!b.TryMegaBufferView(cycle,a,1,0,4));assert(a.bytes==0);
        b.gpuWritesActive=false;b.dirtyState=Buffer::DirtyState::Clean;
        if(direct){b.directTrackedShadowActive=true;assert(!b.TryMegaBufferView(cycle,a,1,0,4));b.directTrackedShadowActive=false;}
        CheckBytes(b,a,b.TryMegaBufferView(cycle,a,1,0,4),0,4);
    }
    {Buffer b{4096};MegaBufferAllocator a;b.sequenceNumber=1;assert(!b.TryMegaBufferView(cycle,a,1,0,4));assert(a.bytes==0);}
    std::cout << "PASS real decision/validation bodies: rejected views, snapshots, reuse, sequencing and eligibility\n";
}
