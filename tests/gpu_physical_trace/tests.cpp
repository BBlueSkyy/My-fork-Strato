// SPDX-License-Identifier: MPL-2.0
#include <gpu/texture/physical_authority_trace.h>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <thread>
#include <span>
#include <future>

using u32=uint32_t;
using namespace skyline::gpu;
using namespace physdiag;
struct Group { std::mutex mutex; U generation{4},guestGeneration{3}; };
struct Format { unsigned vkFormat{122}; };
struct Rect { struct { int x{},y{}; } offset; struct { unsigned width{64},height{64}; } extent; };
struct Guest {
    bool sparseReadOnly{};
    struct { unsigned width{64},height{64},depth{1}; } dimensions;
    Format formatValue;
    Format *format{&formatValue};
    unsigned mipLevelCount{1},layerCount{1};U layerStride{16384};
    struct { unsigned blockHeight{1},blockDepth{1}; } tileConfig;
    std::vector<std::span<uint8_t>> mappings;
};
class Texture {
    template<class T> friend Image physdiag::Describe(T &);
    template<class T> friend State physdiag::Snapshot(T &,bool);
    template<class T> friend void physdiag::Bytes(T &,const char *,const uint8_t *,size_t) noexcept;
    template<class T> friend struct physdiag::DiscardScope;
    template<class T> friend Texel physdiag::GrowthTexel(T &,uint64_t,unsigned,unsigned) noexcept;
    const U physicalTraceIdentity{identities.fetch_add(1)};
    int gpu{};
    bool memoryFreed{};
    std::recursive_mutex stateMutex;
    unsigned dirtyState{2},layout{1};
    U gpuWriteSerial{7},gpuReadbackSerial{6},backingGeneration{2};
    std::atomic<bool> aliasStale{},aliasRetired{};
    std::atomic<std::shared_ptr<Group>> aliasGroup{std::make_shared<Group>()};
    std::shared_ptr<int> cycle{std::make_shared<int>(0)};
    std::span<uint8_t> mirror;
    Format *format;
public:
    std::optional<Guest> guest{Guest{}};
    explicit Texture(std::vector<uint8_t>& bytes):mirror(bytes),format(nullptr){guest->format=&guest->formatValue;format=guest->format;guest->mappings={bytes};}
    std::recursive_mutex &StateForTest(){return stateMutex;}
};
struct View { Texture *texture;struct { unsigned baseMipLevel{},baseArrayLayer{},layerCount{1},levelCount{1}; } range;unsigned type{3}; };
struct TextureSamplerControl {
    enum class MipFilter { None,Nearest,Linear } mipFilter{MipFilter::Nearest};
    float MinLodClamp(){return 1;}float MaxLodClamp(){return 4;}float MipLodBias(){return 0.5f;}
};
struct SamplerFacade {
    struct Pool {
        struct Value {bool didUseTexHeaderBinding{true};std::vector<TextureSamplerControl> texSamplers{TextureSamplerControl{}};} value;
        const Value &Get(){return value;}
    } samplerPool;
    #include "sampler-trace.inc"
};
static std::vector<std::string> lines;
static Image image(U id,U start=100,U size=100) { Image i;i.id=id;i.owner=42;i.format=122;i.count=1;i.maps[0]={start,size};return i; }
int main() {
    static_assert(sizeof(U)==8);
    assert(!Intersection({UINT64_MAX-2,8},{1,3}).size);
    assert(Intersection({100,100},{150,100}).size==50);
    Recorder local;
    local.Reset(42,[](const auto&s){lines.push_back(s);},false);
    assert(!local.Select(image(1),image(2),"equal-maps"));
    auto rgba=image(1);rgba.format=37;
    assert(!local.Select(rgba,image(2,150),"wrong-format"));
    assert(local.Select(image(1),image(2,150),"seed"));
    assert(!local.Select(image(3,1000),image(4,1050),"cannot-reseed"));
    assert(!local.Window(image(5,200,100)).size); // No transitive family expansion.
    for(U i=1;i<=9;++i)local.Emit(image(i),Event{.kind="member"});
    for(unsigned i=0;i<100;++i)local.Emit(image(1),Event{.kind="repeat"});
    for(unsigned i=0;i<100;++i)local.Emit(image(1),Event{.kind="readback-published"});
    assert(!local.active && lines.size()==65); // config + <=64 events including end
    unsigned registrations=0;
    for(const auto&s:lines)registrations+=s.find("\"kind\":\"image\"")!=std::string::npos;
    assert(registrations==8 && lines.back().find("budget_end")!=std::string::npos);
    assert(lines.back().find("\"image_limit\":1")!=std::string::npos);
    // Reproduce the device's activity-only burst, then prove late transfers survive.
    for(bool freeGuest:{false,true}) {
        Recorder horizon;std::vector<std::string> capture;
        horizon.Reset(42,[&](const std::string &s){capture.push_back(s);},freeGuest);
        auto a=image(1);a.format=97;a.width=a.height=128;a.layers=6;
        auto b=image(2,150);b.format=97;b.width=b.height=64;b.layers=6;
        assert(horizon.Select(a,b,"seed"));
        State state{.known=true,.dirty=2,.write=1,.cycle=9};
        for(unsigned i=0;i<10000;++i) {
            ++state.write;
            horizon.Emit(a,Event{.kind="attach-use",.state=state});
            horizon.Emit(a,Event{.kind="write-intent",.state=state});
            horizon.Emit(a,Event{.kind="draw-bound",.state=state});
            horizon.Submitted(9);
            horizon.Emit(a,Event{.kind="readback-request",.state=state});
            horizon.Emit(a,Event{.kind="alias-reconcile",.state=state});
        }
        assert(horizon.active && capture.size()<24);
        state.stale=true;horizon.Emit(a,Event{.kind="write-intent",.state=state});
        for(const auto *kind:{"growth-route","growth-begin","growth-texel","growth-recorded","growth-submitted","consumer-added-levels"})
            horizon.Emit(a,Event{.kind=kind,.state=state});
        horizon.End(42);
        assert(capture.size()<=65 && capture.back().find("\"kind\":\"end\"")!=std::string::npos);
        for(const auto *kind:{"growth-begin","growth-texel","growth-submitted","consumer-added-levels"}) {
            bool found=false;for(const auto &line:capture)found|=line.find(std::string("\"kind\":\"")+kind+"\"")!=std::string::npos;assert(found);
        }
        for(const auto &line:capture)std::cout<<"[GPU-PHYS] "<<line<<'\n';
        std::cerr<<"PASS late-transfer horizon after 60000 repetitive events free="<<freeGuest<<" records="<<capture.size()-1<<"\n";
    }
    // A busy recorder loses a marked event instead of waiting on another caller.
    Recorder contended;
    std::promise<void> entered,proceed;
    auto proceedFuture=proceed.get_future();
    contended.Reset(42,[&](const std::string &s){if(s.find("contended")!=std::string::npos){entered.set_value();proceedFuture.wait();}},false);
    assert(contended.Select(image(1),image(2,150),"seed"));
    std::thread emitter([&]{contended.Emit(image(1),Event{.kind="contended"});});
    entered.get_future().wait();contended.Emit(image(1),Event{.kind="dropped"});
    assert(contended.lost==1);proceed.set_value();emitter.join();contended.End(42);
    assert(UFloat(31U<<6,6)==INFINITY && std::isnan(UFloat((31U<<6)|1,6)));
    assert(UFloat(15U<<6,6)==1 && UFloat(25U<<6,6)==1024);
    assert(UFloat(1,10)>0 && UFloat(0,10)==0);
    std::vector<uint8_t> data(16384,0x5A), other(16384,0x6B);
    Texture texture(data);auto im=Describe(texture), request=im;
    request.maps[0].begin+=4096;request.maps[0].size-=4096;
    recorder.Reset(im.owner,[](const auto&s){std::cout<<"[GPU-PHYS] "<<s<<'\n';},true);
    assert(recorder.Select(im,request,"contained-unmatched"));
    errno=EDOM;Trace(texture,"write-intent");assert(errno==EDOM);
    auto state=Snapshot(texture,true);assert(state.known && state.write==7 && state.readback==6 && state.generation==4);
    assert(!Snapshot(texture,false).known);
    {GPU_PHYS_SCOPE(UnlockedSourceScope context);assert(!Snapshot(texture,true).known);}
    assert(Snapshot(texture,true).known);
    // Real adapters must neither wait for another owner nor invent a valid snapshot.
    std::promise<void> held,release;
    auto released=release.get_future();
    std::thread holder([&]{std::scoped_lock lock{texture.StateForTest()};held.set_value();released.wait();});
    held.get_future().wait();assert(!Snapshot(texture,true).known);release.set_value();holder.join();
    Texture second(other);assert(Describe(second).id!=im.id);
    std::array<uint32_t,2> hdr{(31U<<6)|1U,25U<<6};
    Bytes(texture,"readback-guest-before",reinterpret_cast<const uint8_t*>(hdr.data()),sizeof(hdr));
    const auto h=Hash(data.data()+4096,4096);data[4096]^=1;assert(Hash(data.data()+4096,4096)!=h);
    Bytes(texture,"readback-guest-after");Bytes(texture,"upload-source");
    View view{&texture,{}};std::array<float,4> clear{0.5f,1,0,1};Region(&view,Rect{},"clear-planned","exact-clear",9,clear.data());
    {GPU_PHYS_SCOPE(DiscardScope context{texture});errno=EFAULT;Discard(1234,4096,-1,EINVAL,-1);assert(errno==EFAULT);}
    Trace(texture,"cycle-associated");recorder.Submitted(state.cycle);
    recorder.End(im.owner);
    // Exact texel correspondence for all six faces of mip1 and face0 of mips2..7.
    std::vector<uint8_t> cube(180224*6,0x35), staging(300000,0);
    Texture parent(cube);parent.guest->formatValue.vkFormat=97;parent.guest->dimensions={128,128,1};
    parent.guest->layerCount=6;parent.guest->layerStride=180224;
    auto pi=Describe(parent),pr=pi;pr.maps[0].begin+=131072;pr.maps[0].size-=131072;
    recorder.Reset(pi.owner,[](const auto&s){std::cout<<"[GPU-PHYS] "<<s<<'\n';},false);
    assert(recorder.Select(pi,pr,"seed"));Trace(parent,"attach-use");
    auto child=pi;child.id=900;child.width=child.height=64;child.maps[0]=pr.maps[0];
    recorder.Emit(child,Event{.kind="cycle-associated",.state=State{.known=true,.dirty=2,.write=20,.cycle=555}});
    recorder.Submitted(555);
    auto sampledChild=child;sampledChild.id=901;sampledChild.width=sampledChild.height=32;
    recorder.Emit(sampledChild,Event{.kind="readback-guest-after",.bytes=Range{pr.maps[0].begin,8},.hash=444});
    GrowthRoute(parent,8,true,false);GrowthBegin(parent,1,8);
    const U offsets[]{0,131072,163840,172032,174080,174592,175104,175616};
    U bufferOffset=0;U bufferToken=12345;
    for(unsigned mip=1;mip<8;++mip){const U layerBytes=(128U>>mip)*(128U>>mip)*8;
        for(unsigned layer=0;layer<(mip==1?6U:1U);++layer){
            const U logical=offsets[mip]+layer*180224;U pattern=0x3c003c003c003c00ULL+layer+mip;
            std::memcpy(cube.data()+logical,&pattern,8);auto sample=GrowthTexel(parent,logical,mip,layer);
            assert(sample.known && sample.physical==pi.maps[0].begin+logical && sample.value==pattern);
            std::memcpy(staging.data()+bufferOffset+layer*layerBytes,&pattern,8);
            GrowthPrepared(parent,sample,staging.data(),bufferOffset+layer*layerBytes,mip,layer,bufferToken);
        }bufferOffset+=6*layerBytes;
    }
    auto different=GrowthTexel(parent,131072,1,0);staging[0]^=1;
    GrowthPrepared(parent,different,staging.data(),0,1,0,bufferToken);
    assert(!GrowthTexel(parent,cube.size()-4,1,0).known);
    GrowthRecorded(parent,777,bufferToken,1,8);recorder.Submitted(777);
    parent.guest->mipLevelCount=8;Trace(parent,"growth-published");
    const auto grown=Describe(parent);
    ShaderSet(grown.owner,9,std::array<U,6>{10,11,12,13,14,15});
    View pv{&parent,{0,0,6,8}};SamplerFacade sampler;sampler.TraceLod(&pv,999,0);std::array<View*,1> targets{&pv};
    Consumer(&pv,targets,9);Consumer(&pv,targets,9);recorder.End(pi.owner);
    // ON: rejection due to freed guest, with a reserved slot for an alternate full chain.
    parent.guest->mipLevelCount=1;pi=Describe(parent);pr=pi;pr.maps[0].begin+=131072;pr.maps[0].size-=131072;
    recorder.Reset(pi.owner,[](const auto&s){std::cout<<"[GPU-PHYS] "<<s<<'\n';},true);
    assert(recorder.Select(pi,pr,"seed"));Trace(parent,"attach-use");GrowthRoute(parent,8,false,true);
    for(unsigned i=0;i<6;++i){auto m=pi;m.id=1000+i;m.width=m.height=64U>>i;recorder.Emit(m,Event{.kind="member"});}
    auto tiny=pi;tiny.id=2000;tiny.width=tiny.height=1;assert(!recorder.Window(tiny).size);
    auto alternate=pi;alternate.id=3000;alternate.levels=2;assert(!recorder.Window(alternate).size);alternate.levels=8;assert(recorder.Window(alternate).size);
    recorder.Emit(alternate,Event{.kind="growth-alternate"});recorder.End(pi.owner);
    recorder.Reset(42,[](const auto&s){std::cout<<"[GPU-PHYS] "<<s<<'\n';},false,false);
    assert(!recorder.active);
    std::cerr<<"PASS bounded collector, identity, family selection, sampling/HDR, private adapters, errno, cycle and discard\n";
}
