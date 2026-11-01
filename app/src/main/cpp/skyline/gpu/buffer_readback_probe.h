// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#ifndef STRATO_BUFFER_READBACK_PROBE
#define STRATO_BUFFER_READBACK_PROBE 0
#endif
#if STRATO_BUFFER_READBACK_PROBE
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#endif
namespace skyline::gpu::buffer_readback_probe {
    enum Phase : unsigned {
        DirtyPrepare,
        CurrentPrepare,
        ReadbackWait,
        UploadWait,
        Download,
        Upload,
        Hack,
        Released,
        Count
    };
    inline constexpr std::array<const char *, Count> Names{
        "dirty_prepare", "current_prepare", "readback_wait", "upload_wait", "download", "upload", "hack", "released"};
    struct Value {
        uint64_t calls{}, bytes{}, ns{};
        bool operator==(const Value &) const = default;
    };
#if STRATO_BUFFER_READBACK_PROBE
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    struct Counter {
        std::atomic<uint64_t> calls{}, bytes{}, ns{};
    };
    struct Entry {
        std::atomic<uint64_t> key{}, size{};
        std::array<Counter, Count> phases{};
    };
    inline std::array<Entry, 512> entries{};
    inline std::array<Counter, Count> totals{};
    inline std::atomic<uint64_t> dropped{};
    inline uint64_t Now() {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
    }
    inline Entry *Find(uint64_t id, uint64_t size, bool create = true) {
        if (id == UINT64_MAX) {
            dropped.fetch_add(1);
            return nullptr;
        }
        for (unsigned probe{}; probe < 8; ++probe) {
            auto &e = entries[(id + probe) % entries.size()];
            auto key = e.key.load(std::memory_order_relaxed);
            if (!key) {
                if (!create)
                    return nullptr;
                e.key.compare_exchange_strong(key, id + 1, std::memory_order_relaxed);
            }
            if (e.key.load(std::memory_order_relaxed) == id + 1) {
                e.size.store(size, std::memory_order_relaxed);
                return &e;
            }
        }
        if (create)
            dropped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    inline Value Read(const Counter &c) { return {c.calls.load(), c.bytes.load(), c.ns.load()}; }
    inline void Add(Counter &c, uint64_t ns, uint64_t bytes) {
        c.calls.fetch_add(1, std::memory_order_relaxed);
        c.bytes.fetch_add(bytes, std::memory_order_relaxed);
        c.ns.fetch_add(ns, std::memory_order_relaxed);
    }
    inline void Record(uint64_t id, uint64_t size, Phase phase, uint64_t ns = 0, uint64_t bytes = 0) {
        Add(totals[phase], ns, bytes);
        if (auto e =
                Find(id, size, phase == DirtyPrepare || phase == ReadbackWait || phase == Download || phase == Hack))
            Add(e->phases[phase], ns, bytes);
    }
    struct Scope {
        uint64_t id, size, start, bytes;
        Phase phase;
        bool active;
        Scope(uint64_t id, uint64_t size, Phase phase, uint64_t bytes = 0, bool enabled = true)
            : id(id), size(size), start(enabled ? Now() : 0), bytes(bytes), phase(phase), active(enabled) {}
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        void End() {
            if (active) {
                active = false;
                Record(id, size, phase, Now() - start, bytes);
            }
        }
        ~Scope() { End(); }
    };
#else
    inline void Record(uint64_t, uint64_t, Phase, uint64_t = 0, uint64_t = 0) {}
    struct Scope {
        Scope(uint64_t, uint64_t, Phase, uint64_t = 0, bool = true) {}
        void End() {}
    };
#endif
    class Session {
#if STRATO_BUFFER_READBACK_PROBE
        struct State;
        std::unique_ptr<State> state;
#endif
      public:
        Session(bool fast, bool writes, bool clocks, bool freeGuest);
        void Stop();
        ~Session();
    };
} // namespace skyline::gpu::buffer_readback_probe
