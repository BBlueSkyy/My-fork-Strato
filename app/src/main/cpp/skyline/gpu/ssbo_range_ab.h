// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

#ifndef STRATO_SSBO_RANGE_AB
#define STRATO_SSBO_RANGE_AB 0
#endif
static_assert(STRATO_SSBO_RANGE_AB == 0 || STRATO_SSBO_RANGE_AB == 1);

namespace skyline::gpu::ssbo_range_ab {
    constexpr uint64_t ShaderHash{0xE8B459126941F6CEULL};
    constexpr uint32_t None{std::numeric_limits<uint32_t>::max()};
    constexpr std::array<uint8_t,32> ModuleDigest{
        0xc0,0xce,0x2c,0xdf,0x82,0x97,0x8d,0x80,0x8d,0x47,0xb4,0x9e,0x81,0xca,0xb3,0xdc,
        0x8c,0x2c,0xef,0x96,0xb9,0xa7,0x3f,0x85,0xdb,0xd7,0x37,0xea,0x28,0xf5,0x04,0x41};
    constexpr uint64_t SelectedRange(uint64_t modern, uint64_t paddedProof, unsigned slot, bool certified) {
        return STRATO_SSBO_RANGE_AB == 1 && certified && slot == 0 && paddedProof <= modern
            ? paddedProof : modern;
    }
    constexpr bool ModuleCertified(uint64_t hash, bool compute, bool hashOk, const std::array<uint8_t,32>& digest) {
        return hash == ShaderHash && compute && hashOk && digest == ModuleDigest;
    }
    inline void Max(std::atomic<uint64_t>& x,uint64_t n) {
        auto old{x.load(std::memory_order_relaxed)};
        while(old<n&&!x.compare_exchange_weak(old,n,std::memory_order_relaxed)) {}
    }
    struct Key {
        uint64_t address{}, modern{}, selected{}, view{}, legacyClamp{}, padding{};
        uint32_t slot{}, legacy{};
        std::array<uint32_t,3> groups{};
        bool operator==(const Key&) const = default;
    };
    struct Row {
        std::atomic<unsigned> state{};
        Key key{};
        std::atomic<uint64_t> prepared{}, resolved{}, rangeMin{UINT64_MAX}, rangeMax{}, backingMax{}, preparedBackingMax{}, mega{}, offsetMax{}, truncated{};
    };
    struct Table {
        std::array<Row,128> rows{};
        std::atomic<uint64_t> dropped{};
        uint32_t Prepare(const Key& key, bool truncated) {
            uint64_t hash{key.address ^ (uint64_t{key.legacy}<<32) ^ key.slot ^ (key.groups[0]*131ULL+key.groups[1])};
            hash ^= hash>>33; hash*=0xff51afd7ed558ccdULL; hash^=hash>>33;
            for(unsigned n{};n<rows.size();++n) {
                const uint32_t i{static_cast<uint32_t>((hash+n)%rows.size())};
                auto& r{rows[i]};auto state{r.state.load(std::memory_order_acquire)};
                if(!state) {
                    if(r.state.compare_exchange_strong(state,1,std::memory_order_acq_rel)) {
                        r.key=key;r.state.store(2,std::memory_order_release);state=2;
                    }
                }
                if(state==1) {dropped.fetch_add(1,std::memory_order_relaxed);return None;}
                if(state!=2||!(r.key==key)) continue;
                r.prepared.fetch_add(1,std::memory_order_relaxed);
                r.truncated.fetch_add(truncated,std::memory_order_relaxed);
                return i;
            }
            dropped.fetch_add(1,std::memory_order_relaxed);return None;
        }
        void Resolve(uint32_t i,uint64_t range,uint64_t offset,uint64_t backing,bool mega) {
            if(i>=rows.size()) return;
            auto& r{rows[i]};
            auto old{r.rangeMin.load(std::memory_order_relaxed)};
            while(range<old&&!r.rangeMin.compare_exchange_weak(old,range,std::memory_order_relaxed)) {}
            Max(r.rangeMax,range);Max(r.backingMax,backing);Max(r.offsetMax,offset);
            r.mega.fetch_add(mega,std::memory_order_relaxed);
            r.resolved.fetch_add(1,std::memory_order_release);
        }
    };
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    inline Table table;
}
