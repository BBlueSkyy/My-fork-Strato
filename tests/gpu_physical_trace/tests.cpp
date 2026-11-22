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
struct View { Texture *texture;struct { unsigned baseMipLevel{},baseArrayLayer{},layerCount{1}; } range; };
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
    assert(!local.active && lines.size()==65); // config + <=64 events including end
    unsigned registrations=0;
    for(const auto&s:lines)registrations+=s.find("\"kind\":\"image\"")!=std::string::npos;
    assert(registrations==8 && lines.back().find("budget_end")!=std::string::npos);
    assert(lines.back().find("\"image_limit\":1")!=std::string::npos);
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
    recorder.Reset(42,[](const auto&s){std::cout<<"[GPU-PHYS] "<<s<<'\n';},false,false);
    assert(!recorder.active);
    std::cerr<<"PASS bounded collector, identity, family selection, sampling/HDR, private adapters, errno, cycle and discard\n";
}
