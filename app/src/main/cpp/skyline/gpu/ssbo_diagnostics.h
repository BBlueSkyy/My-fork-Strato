// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace skyline::gpu::ssbo_diagnostics {
    // Experimental observation only. Kind: sized, raw graphics, raw compute
    // inferred, raw compute unknown. Alignment alone is not proof of IR origin.
    constexpr unsigned Classify(bool sized, bool compute, bool inferred) {
        return sized ? 0 : !compute ? 1 : inferred ? 2 : 3;
    }
    constexpr uint64_t Key(uint32_t stage, uint32_t index, uint32_t offset, unsigned kind, bool written) {
        return (uint64_t{stage} << 40) | (uint64_t{index} << 35) | (uint64_t{offset} << 3) | (uint64_t{kind} << 1) | written;
    }
    constexpr uint32_t Stage(uint64_t key) { return static_cast<uint32_t>(key >> 40); }
    constexpr uint32_t Index(uint64_t key) { return static_cast<uint32_t>((key >> 35) & 31); }
    constexpr uint32_t Offset(uint64_t key) { return static_cast<uint32_t>(key >> 3); }
    constexpr unsigned Kind(uint64_t key) { return static_cast<unsigned>((key >> 1) & 3); }
    constexpr bool Written(uint64_t key) { return key & 1; }
    inline void Max(std::atomic<uint64_t> &value, uint64_t next) {
        auto old{value.load(std::memory_order_relaxed)};
        while (old < next && !value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {}
    }
    struct Entry {
        std::atomic<uint64_t> key{}, calls{}, requested{}, effective{}, maxRequested{}, maxEffective{}, maxBacking{}, misses{}, directCalls{};
    };
    struct Table {
        static_assert(std::atomic<uint64_t>::is_always_lock_free);
        std::array<Entry,128> entries{};
        std::atomic<uint64_t> dropped{};
        void Record(uint64_t key, uint64_t requested, uint64_t effective, uint64_t backing, bool direct = false) {
            if (!key) { dropped.fetch_add(1,std::memory_order_relaxed); return; }
            auto slot{static_cast<size_t>((key ^ (key >> 23) ^ (key >> 41)) * 11400714819323198485ULL) % entries.size()};
            for (size_t i{}; i < 16; ++i) {
                auto &e{entries[(slot+i)%entries.size()]};
                uint64_t found{e.key.load(std::memory_order_relaxed)};
                if (!found) {
                    if (e.key.compare_exchange_strong(found,key,std::memory_order_relaxed)) found=key;
                }
                if (found != key) continue;
                e.calls.fetch_add(1,std::memory_order_relaxed);
                e.requested.fetch_add(requested,std::memory_order_relaxed);
                e.effective.fetch_add(effective,std::memory_order_relaxed);
                e.misses.fetch_add(!effective,std::memory_order_relaxed);
                e.directCalls.fetch_add(direct,std::memory_order_relaxed);
                Max(e.maxRequested,requested); Max(e.maxEffective,effective); Max(e.maxBacking,backing);
                return;
            }
            dropped.fetch_add(1,std::memory_order_relaxed);
        }
        Entry *Find(uint64_t key) {
            for (auto &e:entries) if(e.key.load(std::memory_order_relaxed)==key) return &e;
            return nullptr;
        }
    };
    inline Table bindings;
    // All buffers, normal staged synchronization only: not SSBO attribution and
    // not the fast-readback memcpy/direct-import/GPU-copy paths.
    inline std::atomic<uint64_t> uploadCopies{}, uploadBytes{}, downloadCopies{}, downloadBytes{}, dirtyMarks{}, dirtySpanBytes{};
}
