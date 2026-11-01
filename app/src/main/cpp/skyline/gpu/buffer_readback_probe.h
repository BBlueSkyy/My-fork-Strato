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
    enum WaitPart : unsigned { Chain, Submit, Mutex, Driver, Tail, WaitPartCount };
    inline constexpr unsigned WaitMetricCount{WaitPartCount + 7};
    inline constexpr std::array<const char *, WaitMetricCount> WaitNames{"chain",
                                                                         "submit",
                                                                         "mutex",
                                                                         "driver",
                                                                         "semaphore_tail",
                                                                         "samples",
                                                                         "unsubmitted",
                                                                         "already_signalled",
                                                                         "nodes",
                                                                         "same_annotation_cycle",
                                                                         "different_annotation_cycle",
                                                                         "unknown_annotation_cycle"};
    enum class WriteSource : unsigned {
        StorageCompute,
        StorageGraphics,
        ImageCompute,
        ImageGraphics,
        TransformFeedback,
        Query,
        DmaCopy,
        DmaFill,
        InlineUpdate,
        Coalesce,
        Count
    };
    inline constexpr unsigned WriteSourceCount{static_cast<unsigned>(WriteSource::Count)};
    inline constexpr std::array<const char *, WriteSourceCount> WriteNames{
        "storage_compute", "storage_graphics", "image_compute", "image_graphics", "transform_feedback",
        "query",           "dma_copy",         "dma_fill",      "inline_update",  "coalesce"};
    struct WriteSnapshot {
        uint64_t cycle{};
        WriteSource source{WriteSource::Count};
        uint64_t offset{}, size{};
    };
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
        std::array<Counter, WaitMetricCount> waits{};
        std::array<Counter, WriteSourceCount> writes{};
        std::atomic<uint64_t> lastWriteOffset{}, lastWriteSize{};
        std::atomic<uint64_t> sampledSource{WriteSourceCount}, sampledOffset{}, sampledSize{};
        std::atomic<uint64_t> sampleSequence{};
    };
    inline std::array<Entry, 512> entries{};
    inline std::array<Counter, Count> totals{};
    inline std::array<Counter, WaitMetricCount> waitTotals{};
    inline std::array<Counter, WriteSourceCount> writeTotals{};
    inline std::atomic<uint64_t> invalidWriteBounds{}, nextCycleId{1};
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
    inline bool SampleWait(uint64_t id, uint64_t size) {
        auto e = Find(id, size);
        return e && (e->sampleSequence.fetch_add(1, std::memory_order_relaxed) % 16 == 0);
    }
    inline bool RecordWrite(uint64_t id, uint64_t size, WriteSource source, uint64_t offset, uint64_t bound) {
        if (offset > size || bound > size - offset) {
            invalidWriteBounds.fetch_add(1, std::memory_order_relaxed);
            return false; // Diagnostic rejection only, never modifies the real operation.
        }
        auto index = static_cast<unsigned>(source);
        Add(writeTotals[index], 0, bound);
        // Before dirty admission, keep only global annotations to avoid crowding
        // the fixed table with resources that never cause a CPU readback.
        if (auto e = Find(id, size, false)) {
            Add(e->writes[index], 0, bound);
            e->lastWriteOffset.store(offset, std::memory_order_relaxed);
            e->lastWriteSize.store(bound, std::memory_order_relaxed);
        }
        return true;
    }
    struct WaitTrace {
        uint64_t id, size, start, end{}, nodes{};
        uint64_t selectedCycle{};
        WriteSnapshot annotation{};
        unsigned depth{};
        bool enabled, unsubmitted{}, alreadySignalled{};
        std::array<uint64_t, WaitPartCount> parts{};
        WaitTrace(uint64_t id, uint64_t size, bool enabled, uint64_t selectedCycle = 0, WriteSnapshot annotation = {})
            : id{id}, size{size}, start{enabled ? Now() : 0}, selectedCycle{selectedCycle}, annotation{annotation},
              enabled{enabled} {}
        WaitTrace(const WaitTrace &) = delete;
        WaitTrace &operator=(const WaitTrace &) = delete;
        bool Enabled() const { return enabled; }
        uint64_t Elapsed() const { return end ? end - start : Now() - start; }
        void Finish() {
            if (!enabled || end)
                return;
            end = Now();
            auto e = Find(id, size, false);
            if (e) {
                e->sampledSource.store(static_cast<unsigned>(annotation.source), std::memory_order_relaxed);
                e->sampledOffset.store(annotation.offset, std::memory_order_relaxed);
                e->sampledSize.store(annotation.size, std::memory_order_relaxed);
            }
            auto add = [&](unsigned metric, uint64_t calls, uint64_t ns) {
                auto update = [&](Counter &c) {
                    c.calls.fetch_add(calls, std::memory_order_relaxed);
                    c.ns.fetch_add(ns, std::memory_order_relaxed);
                };
                update(waitTotals[metric]);
                if (e)
                    update(e->waits[metric]);
            };
            for (unsigned p{}; p < WaitPartCount; ++p)
                add(p, 1, parts[p]);
            add(WaitPartCount, 1, Elapsed());
            add(WaitPartCount + 1, unsubmitted, 0);
            add(WaitPartCount + 2, alreadySignalled, 0);
            add(WaitPartCount + 3, nodes, 0);
            add(WaitPartCount + 4 +
                    (!selectedCycle || !annotation.cycle ? 2
                     : selectedCycle == annotation.cycle ? 0
                                                         : 1),
                1, 0);
        }
        ~WaitTrace() { Finish(); }
    };
    // Explicit per-call context: simultaneous waiters and nested signal handling
    // cannot borrow another thread's attribution. Only root phases are timed.
    struct FenceWaitScope {
        WaitTrace *trace;
        bool root{};
        unsigned phase{WaitPartCount};
        uint64_t start{};
        explicit FenceWaitScope(WaitTrace *trace) : trace{trace} {
            if (trace) {
                root = trace->depth++ == 0;
                ++trace->nodes;
            }
        }
        bool Active() const { return trace != nullptr; }
        void Entry(bool submitted, bool signalled) {
            if (root) {
                trace->unsubmitted = !submitted && !signalled;
                trace->alreadySignalled = signalled;
            }
        }
        void Next(unsigned next) {
            if (!root)
                return;
            auto now = Now();
            if (phase < WaitPartCount)
                trace->parts[phase] += now - start;
            phase = next;
            start = now;
        }
        ~FenceWaitScope() {
            Next(WaitPartCount);
            if (trace)
                --trace->depth;
        }
    };
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
    inline bool RecordWrite(uint64_t, uint64_t, WriteSource, uint64_t, uint64_t) { return false; }
    inline bool SampleWait(uint64_t, uint64_t) { return false; }
    struct WaitTrace {
        WaitTrace(uint64_t, uint64_t, bool, uint64_t = 0, WriteSnapshot = {}) {}
        bool Enabled() const { return false; }
        void Finish() {}
    };
    struct FenceWaitScope {
        explicit FenceWaitScope(WaitTrace *) {}
        bool Active() const { return false; }
        void Entry(bool, bool) {}
        void Next(unsigned) {}
    };
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
