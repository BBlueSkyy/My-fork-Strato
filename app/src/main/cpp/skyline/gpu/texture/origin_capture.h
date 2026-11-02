// SPDX-License-Identifier: MPL-2.0
#pragma once
// Temporary MK8-only origin recorder. Observes bytes; never changes texture authority.
#include "lifecycle_trace.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <ostream>
#include <vector>

namespace skyline::gpu::texture::diagnostic {
    enum class OriginStage : uint32_t {
        OverlapBefore=1, OverlapAfter, CreatedGuest, GuestBeforeUpload,
        StagingBeforeUpload, HostAfterUpload, BeforeFreeGuest, AfterFreeGuest,
        SampledHost, WriteIntent, ReadbackGuest, AliasRecovery, UploadQueued
    };
    inline constexpr const char *OriginNames[]{
        "unused", "overlap_before", "overlap_after", "created_guest",
        "guest_before_upload", "staging_before_upload", "host_after_upload",
        "before_free_guest", "after_free_guest", "sampled_host",
        "write_intent", "readback_guest", "alias_recovery", "upload_queued"
    };

    class OriginCapture {
        struct Anchor { uintptr_t begin{}; size_t bytes{}; };
        struct Entry {
            OriginStage stage; Snapshot snapshot; uint32_t phase; uint64_t frame;
            std::vector<uint8_t> bytes;
            std::function<std::vector<uint8_t>()> deferred;
        };
        mutable std::mutex mutex;
        std::atomic<bool> active{};
        std::array<Anchor,8> anchors{};
        size_t anchorCount{};
        std::vector<Entry> records;
        std::atomic<uint64_t> missed{}, omitted{};
        static constexpr size_t Limit{320};
        static constexpr size_t MaxBytes{64*1024};

        bool Admit(OriginStage stage, const Snapshot &s, uint32_t phase) {
            if (records.size()>=Limit) { ++omitted; return false; }
            size_t repeated{};
            for (const auto &e:records) {
                if (e.stage!=stage || e.snapshot.id!=s.id || e.phase!=phase) continue;
                // For sampled-host snapshots retain the first observation of
                // each new writer serial instead of two identical early reads.
                if (stage==OriginStage::SampledHost &&
                    e.snapshot.writeSerial==s.writeSerial) return false;
                ++repeated;
            }
            const size_t maxRepeats = stage==OriginStage::WriteIntent ? 6 :
                stage==OriginStage::SampledHost ? 2 : 1;
            if (repeated>=maxRepeats) return false;
            return true;
        }
        static uint64_t Hash(const uint8_t *bytes, size_t count) {
            uint64_t hash{14695981039346656037ULL};
            for(size_t i{};i<count;++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
            return hash;
        }
      public:
        void Start(bool enabled) {
            std::lock_guard lock{mutex};
            active=false; anchors={}; anchorCount=0; records.clear(); missed=0; omitted=0;
            active=enabled;
        }
        bool Enabled() const { return active.load(std::memory_order_relaxed); }
        // CPU *mapping* intervals, not GPU IOVAs. No address or runtime ID is hardcoded.
        void Watch(const void *address, size_t bytes) {
            if (!Enabled() || !address || !bytes || bytes>256*1024) return;
            std::lock_guard lock{mutex};
            const auto begin{reinterpret_cast<uintptr_t>(address)};
            for(size_t i{};i<anchorCount;++i) if(anchors[i].begin==begin && anchors[i].bytes==bytes) return;
            if(anchorCount<anchors.size()) anchors[anchorCount++]={begin,bytes};
            else ++omitted;
        }
        bool Interested(const void *address, size_t bytes) const {
            if(!Enabled() || !address || !bytes) return false;
            const auto begin{reinterpret_cast<uintptr_t>(address)};
            std::lock_guard lock{mutex};
            for(size_t i{};i<anchorCount;++i) {
                const auto a{anchors[i]};
                // Subtractions avoid overflow in the end-address calculation.
                if(begin>=a.begin ? begin-a.begin<a.bytes : a.begin-begin<bytes)
                    return true;
            }
            return false;
        }
        bool CanRecord(OriginStage stage, const Snapshot &s) {
            if(!Enabled()) return false;
            std::lock_guard lock{mutex};
            return Admit(stage,s,lifecycle.CurrentPhase());
        }
        bool Save(OriginStage stage, Snapshot s, const void *data=nullptr, size_t size=0) {
            if(!Enabled() || size>MaxBytes || (size && !data)) return false;
            std::unique_lock lock{mutex,std::try_to_lock};
            if(!lock) { ++missed; return false; }
            const auto phase{lifecycle.CurrentPhase()};
            if(!Admit(stage,s,phase)) return false;
            try {
                Entry entry{stage,s,phase,lifecycle.CurrentFrame(),{},{}};
                if(size) {
                    const auto *ptr{static_cast<const uint8_t *>(data)};
                    entry.bytes.assign(ptr,ptr+size);
                }
                records.emplace_back(std::move(entry));
                return true;
            } catch (...) { ++omitted; return false; }
        }
        bool SaveDeferred(OriginStage stage, Snapshot s, std::function<std::vector<uint8_t>()> fn) {
            if(!Enabled()) return false;
            std::unique_lock lock{mutex,std::try_to_lock};
            if(!lock) { ++missed; return false; }
            const auto phase{lifecycle.CurrentPhase()};
            if(!Admit(stage,s,phase)) return false;
            try {
                records.push_back({stage,s,phase,lifecycle.CurrentFrame(),{},std::move(fn)});
                return true;
            } catch (...) { ++omitted; return false; }
        }
        bool Export(std::ostream &out) {
            std::vector<Entry> copy;
            size_t n{};
            { std::lock_guard lock{mutex}; active=false; copy=records; n=anchorCount; }
            if(copy.empty()) return false;
            out<<"# MK8ORIGIN1; payload=CPU byte copy or explicit deferred GPU read; id/address only valid within one run\n";
            out<<"# observed_intents_are_not_GPU_execution; zero payload with invalid status is NOT a zero-valued image\n";
            out<<"# anchors="<<n<<" missed_lock="<<missed.load()<<" overflow="<<omitted.load()<<'\n';
            out<<"stage,phase,frame,id,address,guest_bytes,image,generation,write_serial,readback_serial,format,width,height,levels,layers,state,flags,detail,mip,face,size,nonzero,fnv1a64,status\n";
            for(auto &e:copy) {
                std::vector<uint8_t> data;
                bool valid{true};
                try {
                    data=e.deferred ? e.deferred() : std::move(e.bytes);
                    valid=!e.deferred || !data.empty();
                } catch (...) { valid=false; }
                const auto &s{e.snapshot};
                auto line=[&](int mip, int face, const uint8_t *ptr, size_t bytes) {
                    size_t nonzero{};
                    for(size_t i{};i<bytes;++i) nonzero+=ptr[i]!=0;
                    out<<OriginNames[static_cast<size_t>(e.stage)]<<','<<e.phase<<','<<e.frame<<','
                       <<s.id<<','<<s.address<<','<<s.bytes<<','<<s.image<<','<<s.generation<<','
                       <<s.writeSerial<<','<<s.readbackSerial<<','<<s.hostFormat<<','
                       <<s.width<<','<<s.height<<','<<s.levels<<','<<s.layers<<','
                       <<s.state<<','<<s.flags<<','<<s.detail<<','<<mip<<','<<face<<','<<bytes<<','
                       <<nonzero<<','<<Hash(ptr,bytes)<<','<<(valid?"ok":"invalid_deferred")<<'\n';
                };
                line(-1,-1,data.data(),data.size());
                // Staging/host is mip-major, six layers per mip. Guest block-linear
                // layout is intentionally not decoded as if it were linear.
                if(valid && (e.stage==OriginStage::StagingBeforeUpload ||
                             e.stage==OriginStage::HostAfterUpload ||
                             e.stage==OriginStage::SampledHost) &&
                   s.width==32 && s.height==32 && s.layers==6 && s.hostFormat==122) {
                    size_t offset{};
                    for(uint32_t mip{};mip<s.levels;++mip) {
                        const size_t side{std::max<size_t>(1,32U>>mip)};
                        const size_t faceBytes{side*side*4};
                        for(uint32_t face{};face<6;++face) {
                            if(offset+faceBytes>data.size()) break;
                            line(static_cast<int>(mip),static_cast<int>(face),data.data()+offset,faceBytes);
                            offset+=faceBytes;
                        }
                    }
                }
            }
            return out.good();
        }
    };
}
