// SPDX-License-Identifier: MPL-2.0
#pragma once

// Disabled builds do not evaluate hook arguments or instantiate the recorder.
#ifndef STRATO_PHYSICAL_AUTHORITY_TRACE
#define GPU_PHYS_TRACE(...) do {} while (0)
#define GPU_PHYS_SCOPE(...)
#else
#define GPU_PHYS_TRACE(...) do { __VA_ARGS__; } while (0)
#define GPU_PHYS_SCOPE(...) __VA_ARGS__

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace skyline::gpu::physdiag {
    using U = uint64_t;
    inline std::atomic<U> identities{1};
    struct ErrnoGuard { int value{errno}; ~ErrnoGuard() { errno = value; } };
    struct Range { U begin{}, size{}; };
    inline Range Intersection(Range a, Range b) {
        if (!a.size || !b.size || a.begin > UINT64_MAX-a.size || b.begin > UINT64_MAX-b.size) return {};
        const auto begin = std::max(a.begin,b.begin), end = std::min(a.begin+a.size,b.begin+b.size);
        return end > begin ? Range{begin,end-begin} : Range{};
    }
    struct Image {
        U id{}, owner{};
        unsigned width{},height{},depth{},format{},levels{},layers{},blockHeight{},blockDepth{};
        U stride{};
        std::array<Range,8> maps{};
        unsigned count{};
        bool truncated{};
    };
    struct State {
        bool known{}, groupKnown{}, freed{}, stale{}, retired{};
        unsigned dirty{},hostFormat{},layout{};
        U write{},readback{},group{},generation{},guestGeneration{},backingGeneration{},cycle{};
    };
    struct Event {
        const char *kind{};
        State state{};
        U peer{},context{},a{},b{},c{},d{};
        int result{},error{},protection{-2};
        std::array<uint32_t,4> clearBits{};bool clearKnown{};
        const char *coverage{"unknown"};
        Range bytes{};
        U logical{},hash{};
        unsigned nan{},excess{},samples{},mip{},baseLayer{},layers{};
    };
    class Recorder {
        std::mutex mutex;
        U owner{};
        Range root{};
        std::array<U,8> images{};
        std::array<Image,8> tracked{};
        std::array<U,8> cycles{};
        unsigned imageCount{},events{};
        std::array<unsigned,9> seen{},kept{};
        std::array<State,8> latest{};
        U suppressed{},criticalSeen{};
        static bool Critical(const char *kind) {
            return std::strcmp(kind,"readback-published")==0 || std::strncmp(kind,"readback-guest-",15)==0 ||
                std::strncmp(kind,"upload",6)==0 || std::strncmp(kind,"free",4)==0 ||
                std::strcmp(kind,"discard-result")==0 || std::strcmp(kind,"authority-transition")==0;
        }
        bool Keep(const Event &e) {
            if(Critical(e.kind)){++criticalSeen;return true;}
            unsigned slot=5;
            if(std::strcmp(e.kind,"write-intent")==0)slot=0;
            else if(std::strcmp(e.kind,"draw-bound")==0)slot=1;
            else if(std::strcmp(e.kind,"attach-use")==0)slot=2;
            else if(std::strcmp(e.kind,"view-request")==0)slot=3;
            else if(std::strcmp(e.kind,"submitted")==0)slot=4;
            else if(std::strcmp(e.kind,"readback-request")==0)slot=6;
            else if(std::strcmp(e.kind,"alias-select")==0)slot=7;
            else if(std::strcmp(e.kind,"alias-reconcile")==0)slot=8;
            ++seen[slot];
            constexpr unsigned quotas[]{4,2,2,1,2,1,2,2,2};
            if(kept[slot]>=quotas[slot]){++suppressed;return false;}
            ++kept[slot];return true;
        }
        bool overflow{};
        std::function<void(const std::string &)> sink;
        void Output(const Image &im, const Event &e) {
            std::ostringstream s;
            s << "{\"v\":1,\"seq\":" << ++events << ",\"kind\":\"" << e.kind
              << "\",\"image\":" << im.id << ",\"root\":[" << root.begin << ',' << root.size
              << "],\"peer\":" << e.peer << ",\"context\":" << e.context
              << ",\"state_known\":" << unsigned(e.state.known) << ",\"dirty\":" << e.state.dirty
              << ",\"freed\":" << unsigned(e.state.freed) << ",\"write\":" << e.state.write
              << ",\"readback\":" << e.state.readback << ",\"stale\":" << unsigned(e.state.stale)
              << ",\"retired\":" << unsigned(e.state.retired) << ",\"group\":" << e.state.group
              << ",\"group_known\":" << unsigned(e.state.groupKnown) << ",\"generation\":" << e.state.generation
              << ",\"guest_generation\":" << e.state.guestGeneration << ",\"backing_generation\":" << e.state.backingGeneration
              << ",\"cycle\":" << e.state.cycle << ",\"host_format\":" << e.state.hostFormat
              << ",\"layout\":" << e.state.layout << ",\"a\":" << e.a << ",\"b\":" << e.b
              << ",\"c\":" << e.c << ",\"d\":" << e.d << ",\"result\":" << e.result
              << ",\"errno\":" << e.error << ",\"protection\":" << e.protection
              << ",\"clear_known\":" << unsigned(e.clearKnown) << ",\"clear\":[" << e.clearBits[0] << ',' << e.clearBits[1] << ',' << e.clearBits[2] << ',' << e.clearBits[3] << "],\"coverage\":\"" << e.coverage
              << "\",\"sample\":[" << e.bytes.begin << ',' << e.bytes.size << ',' << e.logical
              << "],\"hash\":" << e.hash << ",\"nan\":" << e.nan << ",\"excess\":" << e.excess
              << ",\"samples\":" << e.samples << ",\"view\":[" << e.mip << ',' << e.baseLayer << ',' << e.layers << "],\"shape\":[" << im.width << ',' << im.height << ',' << im.depth
              << ',' << im.format << ',' << im.levels << ',' << im.layers << ',' << im.blockHeight << ',' << im.blockDepth
              << ',' << im.stride << "],\"maps_truncated\":" << unsigned(im.truncated) << ",\"maps\":[";
            for (unsigned i=0;i<im.count;++i) { if(i) s << ','; s << '[' << im.maps[i].begin << ',' << im.maps[i].size << ']'; }
            s << "],\"lost\":" << lost.load() << ",\"image_limit\":" << unsigned(overflow)
              << ",\"suppressed\":" << suppressed << ",\"critical_seen\":" << criticalSeen
              << ",\"activity_seen\":[" << seen[0] << ',' << seen[1] << ',' << seen[2] << ',' << seen[3] << ',' << seen[4] << ',' << seen[5] << ',' << seen[6] << ',' << seen[7] << ',' << seen[8] << "]}";
            if (sink) sink(s.str());
        }
        bool Belongs(const Image &im) const {
            if (im.owner != owner || !root.size) return false;
            for(unsigned i=0;i<im.count;++i) if(Intersection(root,im.maps[i]).size) return true;
            return false;
        }
        void EndLocked(const char *reason) {
            active = false;
            if(root.size && events<64) Output({},Event{.kind=reason});
        }
    public:
        std::atomic<bool> active{};
        std::atomic<bool> selected{};
        std::atomic<unsigned> lost{};
        void Reset(U newOwner,std::function<void(const std::string &)> output,bool freeGuest,bool enabled=true) {
            std::scoped_lock lock{mutex};
            owner=newOwner;root={};images={};imageCount=events=0;overflow=false;lost=0;sink=std::move(output);active=enabled;selected=false;tracked={};cycles={};seen={};kept={};latest={};suppressed=criticalSeen=0;
            if(sink) sink(std::string("{\"v\":1,\"kind\":\"config\",\"selector\":\"first_hdr_partial_overlap\",\"sampling\":\"authority-reserved-v2\",\"max_images\":8,\"max_events\":64,\"enabled\":")+(enabled?"1":"0")+",\"free_guest\":"+(freeGuest?"1}":"0}"));
        }
        void End(U who) noexcept {
            try { std::unique_lock lock{mutex,std::try_to_lock}; if(!lock){++lost;return;} if(owner==who && active) { if(!root.size && sink) sink("{\"v\":1,\"kind\":\"no_family\"}"); EndLocked("end"); } } catch(...) { active=false; }
        }
        bool Select(const Image &source,const Image &request,const char *reason) noexcept {
            if(!active) return false;
            try {
                std::unique_lock lock{mutex,std::try_to_lock}; if(!lock){++lost;return false;}
                if(!active || owner!=source.owner || root.size || (source.format!=122 && source.format!=97)) return false;
                bool same=source.count==request.count && !source.truncated && !request.truncated;
                for(unsigned i=0;same && i<source.count;++i) same=source.maps[i].begin==request.maps[i].begin && source.maps[i].size==request.maps[i].size;
                if(same) return false;
                for(unsigned i=0;i<source.count;++i) for(unsigned j=0;j<request.count;++j) {
                    auto hit=Intersection(source.maps[i],request.maps[j]);
                    if(hit.size){root=hit;selected=true;Output(source,Event{.kind="family-seed",.coverage=reason});return true;}
                }
            } catch(...) { active=false; }
            return false;
        }
        void Submitted(U cycle) noexcept {
            if(!active || !selected)return;
            try {std::unique_lock lock{mutex,std::try_to_lock};if(!lock){++lost;return;}
                for(unsigned i=0;i<imageCount && events<63;++i) if(cycles[i] && cycles[i]==cycle) {Event e{.kind="submitted",.context=cycle};if(Keep(e))Output(tracked[i],e);}
                if(events==63)EndLocked("budget_end");
            }catch(...){active=false;}
        }
        Range Window(const Image &im) noexcept {
            if(!active) return {};
            try {std::unique_lock lock{mutex,std::try_to_lock}; if(!lock){++lost;return {};}
                if(!active || !Belongs(im))return {};
                if(imageCount==8 && std::find(images.begin(),images.end(),im.id)==images.end()){overflow=true;return {};}
                return root;
            }catch(...){active=false;return {};}
        }
        void Emit(const Image &im,Event event) noexcept {
            if(!active) return;
            try {
                std::unique_lock lock{mutex,std::try_to_lock}; if(!lock){++lost;return;}
                if(!active || !Belongs(im)) return;
                if(std::find(images.begin(),images.begin()+imageCount,im.id)==images.begin()+imageCount) {
                    if(imageCount==8){overflow=true;return;}
                    images[imageCount]=im.id;tracked[imageCount]=im;++imageCount;
                    if(events>=62){EndLocked("budget_end");return;}
                    Output(im,Event{.kind="image"});
                }
                const auto slot=static_cast<size_t>(std::find(images.begin(),images.begin()+imageCount,im.id)-images.begin());
                if(event.state.known) {
                    cycles[slot]=event.state.cycle;
                    const auto previous=latest[slot];latest[slot]=event.state;
                    if(!Critical(event.kind) && previous.known && (previous.dirty!=event.state.dirty || previous.freed!=event.state.freed ||
                        previous.stale!=event.state.stale || previous.retired!=event.state.retired ||
                        previous.group!=event.state.group || previous.backingGeneration!=event.state.backingGeneration)) {
                        Event change{.kind="authority-transition",.state=event.state,.context=event.context};
                        if(events>=63){EndLocked("budget_end");return;}
                        Keep(change);Output(im,change);
                        if(events==63){EndLocked("budget_end");return;}
                    }
                }
                if(!Keep(event))return;
                if(events>=63){EndLocked("budget_end");return;}
                Output(im,event);
                if(events==63) EndLocked("budget_end");
            } catch(...) { active=false; }
        }
    };
    inline Recorder recorder;

    template<class T> Image Describe(T &t) {
        Image im;
        im.id=t.physicalTraceIdentity;im.owner=reinterpret_cast<U>(&t.gpu);
        if(!t.guest || t.guest->sparseReadOnly) return im;
        const auto &g=*t.guest;
        im.width=g.dimensions.width;im.height=g.dimensions.height;im.depth=g.dimensions.depth;
        im.format=static_cast<unsigned>(g.format->vkFormat);im.levels=g.mipLevelCount;im.layers=g.layerCount;
        im.blockHeight=g.tileConfig.blockHeight;im.blockDepth=g.tileConfig.blockDepth;im.stride=g.layerStride;
        for(const auto &m:g.mappings) {
            if(im.count==8){im.truncated=true;break;}
            im.maps[im.count++]={reinterpret_cast<U>(m.data()),m.size()};
        }
        return im;
    }
    inline thread_local bool resourceSnapshotAllowed=true;
    struct UnlockedSourceScope {
        bool previous{resourceSnapshotAllowed};
        UnlockedSourceScope(){resourceSnapshotAllowed=false;}
        ~UnlockedSourceScope(){resourceSnapshotAllowed=previous;}
    };
    template<class T> State Snapshot(T &t,bool owned) {
        State s;
        if(!owned || !resourceSnapshotAllowed) return s; // Never read mutable resource state from a manager scan.
        std::unique_lock lock{t.stateMutex,std::try_to_lock}; if(!lock) return s;
        s.known=true;s.dirty=static_cast<unsigned>(t.dirtyState);s.freed=t.memoryFreed;
        s.write=t.gpuWriteSerial;s.readback=t.gpuReadbackSerial;s.stale=t.aliasStale.load();s.retired=t.aliasRetired.load();
        s.hostFormat=static_cast<unsigned>(t.format->vkFormat);s.layout=static_cast<unsigned>(t.layout);
        s.backingGeneration=t.backingGeneration;s.cycle=reinterpret_cast<U>(t.cycle.get());
        if(auto group=t.aliasGroup.load()) {
            s.group=reinterpret_cast<U>(group.get());
            std::unique_lock metadata{group->mutex,std::try_to_lock};
            if(metadata){s.groupKnown=true;s.generation=group->generation;s.guestGeneration=group->guestGeneration;}
        }
        return s;
    }
    template<class T> void Trace(T &t,const char *kind,bool owned=true,U context=0) noexcept {
        ErrnoGuard error;
        if(!recorder.active || !recorder.selected) return;
        try { auto im=Describe(t); if(recorder.Window(im).size) recorder.Emit(im,Event{.kind=kind,.state=Snapshot(t,owned),.context=context}); } catch(...) {}
    }
    template<class G,class T> void Candidate(const G &guest,T &t,const char *reason) noexcept {
        ErrnoGuard error;
        if(!recorder.active) return;
        try {
            if(!t.guest) return;
            const auto format=static_cast<unsigned>(t.guest->format->vkFormat);
            if(!recorder.selected && format!=122 && format!=97) return;
            auto im=Describe(t), request=im;request.count=0;request.truncated=false;
            request.width=guest.dimensions.width;request.height=guest.dimensions.height;request.depth=guest.dimensions.depth;
            request.format=static_cast<unsigned>(guest.format->vkFormat);
            for(const auto &m:guest.mappings){if(request.count==8){request.truncated=true;break;}request.maps[request.count++]={reinterpret_cast<U>(m.data()),m.size()};}
            recorder.Select(im,request,reason);
            if(recorder.Window(im).size) recorder.Emit(im,Event{.kind=reason,.a=request.width,.b=request.height,.c=request.count?request.maps[0].begin:0,.d=request.count?request.maps[0].size:0});
        } catch(...) {}
    }
    inline U Hash(const uint8_t *p,size_t size) { U h=14695981039346656037ULL;for(size_t i=0;i<size;++i){h^=p[i];h*=1099511628211ULL;}return h; }
    inline float UFloat(unsigned value,unsigned mantissa) {
        const unsigned exponent=value>>mantissa, fraction=value&((1U<<mantissa)-1);
        if(exponent==31) return fraction?NAN:INFINITY;
        return std::ldexp(static_cast<float>(exponent?fraction+(1U<<mantissa):fraction),int(exponent?exponent:1)-15-int(mantissa));
    }
    template<class T> void Bytes(T &t,const char *kind,const uint8_t *linear=nullptr,size_t linearSize=0) noexcept {
        ErrnoGuard error;
        if(!recorder.active || !recorder.selected) return;
        try {
            auto im=Describe(t);auto root=recorder.Window(im);if(!root.size)return;
            Event e{.kind=kind,.state=Snapshot(t,true)};
            size_t offset=0;
            for(unsigned i=0;i<im.count;++i){auto hit=Intersection(root,im.maps[i]);if(hit.size){
                e.logical=offset+(hit.begin-im.maps[i].begin);
                if(e.logical<t.mirror.size()) { e.bytes={hit.begin,std::min<U>({hit.size,4096,t.mirror.size()-e.logical})};e.hash=Hash(t.mirror.data()+e.logical,e.bytes.size); }
                break;
            }offset+=im.maps[i].size;}
            // Diagnostic prefix statistics only, not a whole-image HDR verdict.
            if(linear && (im.format==122 || im.format==97)) {
                const size_t step=im.format==122?4:8;
                for(size_t i=0;i<std::min<size_t>(linearSize/step,256);++i) {
                    for(unsigned c=0;c<(im.format==122?3U:4U);++c){float value;
                        if(im.format==122){uint32_t p;std::memcpy(&p,linear+i*step,4);value=UFloat(c==0?p&2047:c==1?(p>>11)&2047:p>>22,c==2?5:6);}
                        else {uint16_t p;std::memcpy(&p,linear+i*step+c*2,2);value=(p&0x8000?-1:1)*UFloat(p&0x7fff,10);}
                        e.nan+=std::isnan(value);e.excess+=std::isfinite(value)&&value>1000;++e.samples;
                    }
                }
            }
            recorder.Emit(im,e);
        } catch(...) {}
    }
    template<class V,class R> void Region(V *view,const R &rect,const char *kind,const char *coverage,U context=0,const void *clear=nullptr) noexcept {
        ErrnoGuard error;if(!view || !recorder.active || !recorder.selected)return;
        try {auto &t=*view->texture;auto im=Describe(t);if(!recorder.Window(im).size)return;
            Event e{.kind=kind,.state=Snapshot(t,true),.context=context,.a=static_cast<U>(rect.offset.x),.b=static_cast<U>(rect.offset.y),.c=rect.extent.width,.d=rect.extent.height,.coverage=coverage};
            if(clear){std::memcpy(e.clearBits.data(),clear,16);e.clearKnown=true;}
            e.mip=view->range.baseMipLevel;e.baseLayer=view->range.baseArrayLayer;e.layers=view->range.layerCount;
            recorder.Emit(im,e);
        }catch(...){}
    }
    struct DiscardContext { Image im{};State state{};U mirror{}; };
    inline thread_local DiscardContext discardContext;
    template<class T> struct DiscardScope {
        DiscardContext previous{discardContext};
        explicit DiscardScope(T &t){
            discardContext={};
            if(!recorder.active || !recorder.selected)return;
            auto im=Describe(t);if(recorder.Window(im).size)discardContext={im,Snapshot(t,true),reinterpret_cast<U>(t.mirror.data())};
        }
        ~DiscardScope(){discardContext=previous;}
    };
    inline void Discard(U begin,U size,int result,int error,int protection) noexcept {
        ErrnoGuard preserve;
        recorder.Emit(discardContext.im,Event{.kind="discard-result",.state=discardContext.state,.a=begin,.b=size,.c=discardContext.mirror,.result=result,.error=error,.protection=protection});
    }
}
#endif
