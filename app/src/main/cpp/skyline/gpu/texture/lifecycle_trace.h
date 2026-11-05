// SPDX-License-Identifier: MPL-2.0
#pragma once

// Temporary MK8 diagnostics. Independent of Vulkan/Android; never owns resources.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <vector>

namespace skyline::gpu::texture::diagnostic {
    enum class Event : uint8_t {
        Create, Destroy, View, SampleRead, StorageRead, StorageWriteIntent,
        RenderTargetIntent, CopyRead, CopyWriteIntent, WriteRecorded,
        HostSyncRequest, InlineSyncRequest, HostGpuOwnership, InlineGpuOwnership,
        UploadBegin, UploadRecorded, FreeGuestAttempt, FreeGuest,
        GuestSyncRequest, ReadbackSkipped, ReadbackBegin, ReadbackComplete,
        CpuReadTrap, CpuWriteTrap, CpuDirty, AliasMember, AliasRecover,
        Mapping, Overlap, CopyFromRead, CopyFromRecorded, ClearRequest, DescriptorIova, NullIndex, NullFormat, NullMapping, Count
    };
    inline constexpr const char *Names[]{
        "Create", "Destroy", "View", "SampleRead", "StorageRead", "StorageWriteIntent",
        "RenderTargetIntent", "CopyRead", "CopyWriteIntent", "WriteRecorded",
        "HostSyncRequest", "InlineSyncRequest", "HostGpuOwnership", "InlineGpuOwnership",
        "UploadBegin", "UploadRecorded", "FreeGuestAttempt", "FreeGuest",
        "GuestSyncRequest", "ReadbackSkipped", "ReadbackBegin", "ReadbackComplete",
        "CpuReadTrap", "CpuWriteTrap", "CpuDirty", "AliasMember", "AliasRecover", "Mapping",
        "Overlap", "CopyFromRead", "CopyFromRecorded", "ClearRequest", "DescriptorIova", "NullIndex", "NullFormat", "NullMapping"
    };
    static_assert(std::size(Names) == static_cast<size_t>(Event::Count));
    constexpr bool UsesIdentityMapping(Event event) {
        return event == Event::StorageRead || event == Event::StorageWriteIntent;
    }
    struct Snapshot {
        uint64_t id{}, address{}, bytes{}, peer{}, alias{}, cycle{}, writeSerial{}, readbackSerial{}, generation{}, image{};
        uint32_t guestFormat{}, hostFormat{}, width{}, height{}, depth{}, levels{}, layers{};
        uint32_t state{}, flags{}, layout{}, detail{}, mip{}, levelCount{}, layer{}, layerCount{}, aspect{}, swizzle{}, viewType{};
    };
    struct Token {
        uint64_t epoch{};
        uint32_t phase{~0U};
        std::array<uint8_t, static_cast<size_t>(Event::Count)> repeats{};
    };
    template<size_t Capacity = 8192> class Recorder {
        struct Entry { Snapshot snapshot; uint64_t sequence, frame, micros; Event event; uint32_t phase; };
        std::atomic<bool> enabled{}, available{};
        std::atomic<uint32_t> phase{};
        std::atomic<uint64_t> frame{}, nextId{1};
        std::array<std::atomic<uint64_t>, 4> contention{}, overflow{}, limited{}, snapshotMiss{};
        std::mutex mutex;
        std::unique_ptr<Entry[]> records;
        std::array<size_t, 4> counts{};
        std::array<uint64_t,4> markerFrames{}, markerMicros{};
        uint64_t epoch{}, sequence{};
        std::chrono::steady_clock::time_point start;
      public:
        void Start(bool active) { // Before guest threads start; export/phase use this mutex too.
            std::lock_guard lock{mutex};
            enabled = false; available = false;
            counts = {}; markerFrames={}; markerMicros={};
            for (size_t i{}; i<4; ++i) { contention[i]=0; overflow[i]=0; limited[i]=0; snapshotMiss[i]=0; }
            phase=0; frame=0; sequence=0; ++epoch;
            start=std::chrono::steady_clock::now();
            if (active && !records) records=std::make_unique<Entry[]>(Capacity*4);
            available=active; enabled=active;
        }
        bool Available() const { return available.load(); }
        void Freeze() { std::lock_guard lock{mutex}; enabled=false; }
        bool Enabled() const { return enabled.load(std::memory_order_relaxed); }
        uint64_t NewId() { return Enabled() ? nextId.fetch_add(1, std::memory_order_relaxed) : 0; }
        uint64_t CurrentFrame() const { return frame.load(std::memory_order_relaxed); }
        uint32_t CurrentPhase() const { return phase.load(std::memory_order_relaxed); }
        void Frame() { if (Enabled()) frame.fetch_add(1, std::memory_order_relaxed); }
        void MissSnapshot() { if (Enabled()) ++snapshotMiss[phase.load()]; }
        bool SetPhase(uint32_t value) {
            if (!Enabled() || value>3) return false;
            std::lock_guard lock{mutex};
            if (!Enabled()) return false;
            // Monotonic phases: never relabel or overwrite an earlier observation.
            if (value<phase.load()) return false;
            if (value==phase.load()) return true;
            phase=value;
            markerFrames[value]=frame.load();
            markerMicros[value]=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            return true;
        }
        bool Record(Token &token, Event event, const Snapshot &snapshot) {
            if (!Enabled()) return false;
            std::unique_lock lock{mutex, std::try_to_lock};
            if (!lock) { ++contention[phase.load()]; return false; }
            if (!Enabled()) return false;
            const auto p{phase.load()};
            if (token.epoch!=epoch || token.phase!=p) { token.epoch=epoch; token.phase=p; token.repeats={}; }
            auto &repeat{token.repeats[static_cast<size_t>(event)]};
            // Preserve the first two examples per event/texture/phase; report all omitted examples.
            if (repeat==(event == Event::Mapping ? 16 : 2)) { ++limited[p]; return false; }
            if (counts[p]==Capacity) { ++overflow[p]; return false; }
            ++repeat;
            const auto us{std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count()};
            records[p*Capacity+counts[p]++]={snapshot,++sequence,frame.load(),static_cast<uint64_t>(us),event,p};
            return true;
        }
        size_t Count() { std::lock_guard lock{mutex}; size_t n{}; for(auto c:counts)n+=c; return n; }
        void Export(std::ostream &out) {
            std::vector<Entry> copy;
            std::array<std::array<uint64_t,4>,4> stats{};
            std::array<uint64_t,4> markers{}, times{};
            {
                std::lock_guard lock{mutex};
                markers=markerFrames; times=markerMicros;
                for(size_t p{};p<4;++p) {
                    if (records) copy.insert(copy.end(), records.get()+p*Capacity, records.get()+p*Capacity+counts[p]);
                    stats[p]={contention[p].load(),overflow[p].load(),limited[p].load(),snapshotMiss[p].load()};
                }
            }
            out << "# MK8 texture lifecycle v1; state=0 Clean/1 CpuDirty/2 GpuDirty; addresses=guest CPU mappings, not IOVA\n";
            out << "# flags: 1=freed 2=aliasStale 4=aliasReadOnly 8=aliasRetired 16=stateOnly 32=metadataOnly; declared writes are intent, not execution proof\n";
            for(size_t p{};p<4;++p) out << "# phase="<<p<<" contention="<<stats[p][0]<<" overflow="<<stats[p][1]<<" limited="<<stats[p][2]<<" snapshot_miss="<<stats[p][3]<<" marker_frame="<<markers[p]<<" marker_us="<<times[p]<<'\n';
            out << "seq,phase,frame,us,event,id,address,bytes,peer,alias,cycle,write_serial,readback_serial,generation,guest_format,host_format,width,height,depth,levels,layers,state,flags,layout,detail,mip,level_count,layer,layer_count,image,aspect,swizzle,view_type\n";
            for(const auto &r:copy) {
                const auto &s{r.snapshot};
                out<<r.sequence<<','<<r.phase<<','<<r.frame<<','<<r.micros<<','<<Names[static_cast<size_t>(r.event)]<<','
                   <<s.id<<','<<s.address<<','<<s.bytes<<','<<s.peer<<','<<s.alias<<','<<s.cycle<<','<<s.writeSerial<<','<<s.readbackSerial<<','<<s.generation<<','
                   <<s.guestFormat<<','<<s.hostFormat<<','<<s.width<<','<<s.height<<','<<s.depth<<','<<s.levels<<','<<s.layers<<','<<s.state<<','<<s.flags<<','<<s.layout<<','<<s.detail<<','
                   <<s.mip<<','<<s.levelCount<<','<<s.layer<<','<<s.layerCount<<','<<s.image<<','<<s.aspect<<','<<s.swizzle<<','<<s.viewType<<'\n';
            }
        }
    };
    inline Recorder<> lifecycle;
}
