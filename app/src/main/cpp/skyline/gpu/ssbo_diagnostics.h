// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace skyline::gpu::ssbo_diagnostics {
    // Temporary, bounded observation. No resource lifetime or Vulkan decisions.
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    inline void Max(std::atomic<uint64_t> &value, uint64_t next) {
        auto old{value.load(std::memory_order_relaxed)};
        while (old < next && !value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {}
    }
    inline void Min(std::atomic<uint64_t> &value, uint64_t next) {
        auto old{value.load(std::memory_order_relaxed)};
        while (old > next && !value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {}
    }
    struct Dispatch {
        uint64_t shader{}, pipeline{};
        std::array<uint32_t, 3> groups{};
    };
    struct BindingKey {
        Dispatch dispatch;
        uint64_t buffer{};
        uint32_t cbuf{}, offset{}, kind{};
        bool written{};
        bool operator==(const BindingKey &other) const {
            return dispatch.shader == other.dispatch.shader && dispatch.pipeline == other.dispatch.pipeline &&
                dispatch.groups == other.dispatch.groups && buffer == other.buffer && cbuf == other.cbuf &&
                offset == other.offset && kind == other.kind && written == other.written;
        }
        uint64_t Hash() const {
            uint64_t h{dispatch.shader};
            const auto add = [&](uint64_t x) { h = (h ^ x) * 1099511628211ULL; };
            add(dispatch.pipeline); add(buffer); add(cbuf); add(offset); add(kind); add(written);
            for (auto g : dispatch.groups) add(g);
            return h;
        }
    };
    struct BindingEntry {
        // 0 empty, 1 being initialized, 2 published. Key is immutable after release.
        std::atomic<unsigned> state{};
        BindingKey key{};
        std::atomic<uint64_t> minGuest{UINT64_MAX};
        std::atomic<uint64_t> maxGuest{}, maxViewOffset{};
        std::atomic<uint64_t> revision{}, calls{}, requested{}, viewed{}, maxRequest{}, maxView{}, maxBacking{},
            updateNs{}, attachNs{}, megaCalls{};
    };
    struct BindingTable {
        std::array<BindingEntry, 2048> entries{};
        std::atomic<uint64_t> dropped{};
        BindingEntry *Record(const BindingKey &key, uint64_t requested, uint64_t view, uint64_t backing,
                             uint64_t updateNs, uint64_t attachNs, uint64_t guestAddress = 0, uint64_t viewOffset = 0) {
            const auto slot{key.Hash() % entries.size()};
            for (size_t probe{}; probe < 32; ++probe) {
                auto &e{entries[(slot + probe) % entries.size()]};
                unsigned state{e.state.load(std::memory_order_acquire)};
                if (!state && e.state.compare_exchange_strong(state, 1, std::memory_order_acquire)) {
                    e.key = key;
                    e.state.store(2, std::memory_order_release);
                    state = 2;
                }
                if (state != 2 || !(e.key == key)) continue;
                e.requested.fetch_add(requested, std::memory_order_relaxed);
                e.viewed.fetch_add(view, std::memory_order_relaxed);
                e.updateNs.fetch_add(updateNs, std::memory_order_relaxed);
                e.attachNs.fetch_add(attachNs, std::memory_order_relaxed);
                Min(e.minGuest, guestAddress); Max(e.maxGuest, guestAddress); Max(e.maxViewOffset, viewOffset);
                Max(e.maxRequest, requested); Max(e.maxView, view); Max(e.maxBacking, backing);
                e.calls.fetch_add(1, std::memory_order_relaxed);
                e.revision.fetch_add(1, std::memory_order_release);
                return &e;
            }
            dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    };
    struct BufferStats {
        std::atomic<uint64_t> seen{}, revision{}, bytes{}, guestStart{}, direct{}, computeSeen{},
            uploads{}, uploadBytes{}, uploadNs{}, downloads{}, downloadBytes{}, downloadNs{},
            waits{}, waitNs{}, trapWaits{}, trapWaitNs{}, fastCopies{}, fastBytes{}, coalesceCopies{}, coalesceBytes{}, coalesceNs{}, successor{}, dirtyMarks{}, dirtySpan{}, megaCopies{}, megaBytes{};
    };
    struct BufferTable {
        // BufferManager IDs are monotonic, including replaced/coalesced buffers.
        // Never evict or reuse observations; overflow is explicit.
        std::array<BufferStats, 32768> entries{};
        std::atomic<uint64_t> dropped{}, created{};
        BufferStats *Get(uint64_t id) {
            return id < entries.size() ? &entries[id] : nullptr;
        }
        void Init(uint64_t id, uint64_t bytes, uint64_t guestStart, bool direct) {
            created.fetch_add(1, std::memory_order_relaxed);
            auto *e{Get(id)};
            if (!e) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
            e->bytes.store(bytes, std::memory_order_relaxed);
            e->guestStart.store(guestStart, std::memory_order_relaxed);
            e->direct.store(direct, std::memory_order_relaxed);
            e->seen.store(1, std::memory_order_release);
        }
        void Bind(uint64_t id) {
            if (auto *e{Get(id)}; e && !e->computeSeen.exchange(1, std::memory_order_relaxed))
                e->revision.fetch_add(1, std::memory_order_release);
        }
        void Copy(uint64_t id, bool upload, uint64_t bytes, uint64_t ns) {
            if (auto *e{Get(id)}) {
                (upload ? e->uploads : e->downloads).fetch_add(1, std::memory_order_relaxed);
                (upload ? e->uploadBytes : e->downloadBytes).fetch_add(bytes, std::memory_order_relaxed);
                (upload ? e->uploadNs : e->downloadNs).fetch_add(ns, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
        void Wait(uint64_t id, uint64_t ns) {
            if (auto *e{Get(id)}) {
                e->waits.fetch_add(1, std::memory_order_relaxed);
                e->waitNs.fetch_add(ns, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
        void TrapWait(uint64_t id, uint64_t ns) {
            if (auto *e{Get(id)}) {
                e->trapWaits.fetch_add(1, std::memory_order_relaxed);
                e->trapWaitNs.fetch_add(ns, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
        void Fast(uint64_t id, uint64_t bytes) {
            if (auto *e{Get(id)}) {
                e->fastCopies.fetch_add(1, std::memory_order_relaxed);
                e->fastBytes.fetch_add(bytes, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
        void Coalesce(uint64_t dst, uint64_t bytes, uint64_t ns) {
            if (auto *e{Get(dst)}) {
                e->coalesceCopies.fetch_add(1, std::memory_order_relaxed);
                e->coalesceBytes.fetch_add(bytes, std::memory_order_relaxed);
                e->coalesceNs.fetch_add(ns, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
        void Link(uint64_t src, uint64_t dst) {
            if (auto *e{Get(src)}) {
                e->successor.store(dst + 1, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
        void ResolveComputeRelated(std::array<bool, 32768> &related) {
            // IDs increase on coalescence: first propagate consumers forward,
            // then include predecessors sharing their backing, in linear time.
            related.fill(false);
            for (size_t i{}; i < entries.size(); ++i) {
                related[i] = related[i] || entries[i].computeSeen.load(std::memory_order_relaxed);
                const auto next{entries[i].successor.load(std::memory_order_relaxed)};
                if (related[i] && next && next - 1 > i && next - 1 < related.size()) related[next - 1] = true;
            }
            for (size_t i{entries.size()}; i-- > 0;) {
                const auto next{entries[i].successor.load(std::memory_order_relaxed)};
                related[i] = related[i] || (next && next - 1 > i && next - 1 < related.size() && related[next - 1]);
            }
        }
        void Dirty(uint64_t id, uint64_t bytes) {
            if (auto *e{Get(id)}) {
                e->dirtyMarks.fetch_add(1, std::memory_order_relaxed);
                e->dirtySpan.fetch_add(bytes, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
        void Mega(uint64_t id, uint64_t bytes) {
            if (auto *e{Get(id)}) {
                e->megaCopies.fetch_add(1, std::memory_order_relaxed);
                e->megaBytes.fetch_add(bytes, std::memory_order_relaxed);
                e->revision.fetch_add(1, std::memory_order_release);
            }
        }
    };
    inline BindingTable bindings;
    inline BufferTable buffers;
    inline std::atomic<uint64_t> compiledPipelines{}, diagnosticPipelinesDropped{};
}
