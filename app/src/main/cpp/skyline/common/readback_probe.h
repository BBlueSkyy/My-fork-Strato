// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>
#if STRATO_READBACK_DIAGNOSTICS
#include "readback_fault.h"
#endif
#ifndef STRATO_READBACK_DIAGNOSTICS
#define STRATO_READBACK_DIAGNOSTICS 0
#endif
namespace skyline::readback_probe {
    enum class Metric : unsigned {
        BufferReadTrap,
        BufferWriteTrap,
        TextureReadTrap,
        TextureWriteTrap,
        TrapDispatch,
        TrapPrepare,
        BufferReadback,
        BufferUpload,
        BufferCopy,
        TextureReadback,
        TextureUpload,
        TextureConvert,
        ProducerWait,
        TransferWait,
        FenceWait,
        SubmitWait,
        FenceMutex,
        DriverWait,
        QueueMutex,
        QueueSubmit,
        ExecutorSubmit,
        PresentWork,
        PresentBackpressure,
        PresentFence,
        AcquireImage,
        PresentCall,
        BufferHack,
        TextureHack,
        CpuDirty,
        GpuDirty,
        AliasReconcile,
        Invalidate,
        Reprotect,
        FrameQueued,
        FramePresented,
        GuestTimestampStep,
        TextureDownload,
        ReadbackRequest,
        TrapHandled,
        TrapUnhandled,
        TrapRetry,
        Count
    };
    inline constexpr std::array Names{"buf_read_trap",
                                      "buf_write_trap",
                                      "tex_read_trap",
                                      "tex_write_trap",
                                      "trap",
                                      "prepare",
                                      "buf_readback",
                                      "buf_upload",
                                      "buf_memcpy",
                                      "tex_readback",
                                      "tex_upload",
                                      "tex_convert",
                                      "producer_wait",
                                      "transfer_wait",
                                      "fence_root",
                                      "submit_wait",
                                      "fence_mutex",
                                      "driver_wait",
                                      "queue_mutex",
                                      "queue_submit",
                                      "executor_submit",
                                      "present_work",
                                      "present_backpressure",
                                      "present_fence",
                                      "acquire",
                                      "present_call",
                                      "buf_hack",
                                      "tex_hack",
                                      "cpu_dirty",
                                      "gpu_dirty",
                                      "alias_reconcile",
                                      "invalidate",
                                      "reprotect",
                                      "present_request_dt",
                                      "present_return_dt",
                                      "guest_timestamp_dt",
                                      "tex_download_bytes",
                                      "readback_request",
                                      "trap_handled",
                                      "trap_unhandled",
                                      "trap_retry"};
    static_assert(Names.size() == static_cast<unsigned>(Metric::Count));
    struct Value {
        uint64_t calls{}, bytes{}, ns{}, max{};
        std::array<uint64_t, 8> histogram{};
        std::array<uint64_t, 4> domainNs{};
    };
    enum class Domain : unsigned { None, Buffer, Texture, Presentation };
    struct Sample {
        uint64_t sequence{}, start{}, ns{}, resource{}, bytes{}, tid{}, cycle{};
        Metric metric{};
        Domain domain{};
    };
#if STRATO_READBACK_DIAGNOSTICS
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    inline uint64_t Now() noexcept {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
    }
    namespace detail {
        struct AtomicValue {
            std::atomic<uint64_t> calls{}, bytes{}, ns{}, max{};
            std::array<std::atomic<uint64_t>, 8> histogram{};
            std::array<std::atomic<uint64_t>, 4> domainNs{};
        };
        inline std::array<AtomicValue, Names.size()> metrics{};
        struct Thread {
            std::atomic<uint64_t> tid{}, trapStart{}, trapNs{}, resource{}, domain{};
        };
        inline std::array<Thread, 64> threads{};
        inline std::atomic<uint64_t> nextThread{}, threadOverflow{};
        inline thread_local Thread *thread{};
        inline thread_local bool registered{};
        inline thread_local uint64_t resource{}, cycle{};
        inline thread_local Domain domain{};
        inline thread_local std::array<unsigned, Names.size()> depth{};
        inline Thread *GetThread() noexcept {
            if (!registered) {
                registered = true;
                auto slot = nextThread.fetch_add(1, std::memory_order_relaxed);
                if (slot < threads.size()) {
                    thread = &threads[slot];
                    thread->tid.store(syscall(SYS_gettid), std::memory_order_release);
                } else
                    threadOverflow.fetch_add(1, std::memory_order_relaxed);
            }
            return thread;
        }
        struct Slot {
            std::atomic_flag busy{};
            Sample sample{};
        };
        inline std::array<Slot, 128> samples{};
        inline std::atomic<uint64_t> sampleSequence{}, sampleDrops{};
        inline std::atomic<uint64_t> frameQueued{}, framePresented{}, guestTimestamp{};
        inline void Max(std::atomic<uint64_t> &value, uint64_t next) noexcept {
            auto old = value.load(std::memory_order_relaxed);
            while (old < next && !value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {
            }
        }
    } // namespace detail
    inline void RecordAs(Metric metric, uint64_t ns, uint64_t bytes, Domain domain) noexcept {
        auto &v = detail::metrics[static_cast<unsigned>(metric)];
        v.domainNs[unsigned(domain)].fetch_add(ns, std::memory_order_relaxed);
        v.calls.fetch_add(1, std::memory_order_relaxed);
        v.bytes.fetch_add(bytes, std::memory_order_relaxed);
        v.ns.fetch_add(ns, std::memory_order_relaxed);
        detail::Max(v.max, ns);
        constexpr std::array<uint64_t, 7> bounds{1000, 10000, 100000, 1000000, 4000000, 16000000, 64000000};
        unsigned bucket{};
        while (bucket < bounds.size() && ns > bounds[bucket])
            ++bucket;
        v.histogram[bucket].fetch_add(1, std::memory_order_relaxed);
    }
    inline void Record(Metric metric, uint64_t ns = 0, uint64_t bytes = 0) noexcept {
        RecordAs(metric, ns, bytes, detail::domain);
    }
    inline Value Read(Metric metric) noexcept {
        auto &v = detail::metrics[static_cast<unsigned>(metric)];
        Value result;
        result.calls = v.calls.load();
        result.bytes = v.bytes.load();
        result.ns = v.ns.load();
        result.max = v.max.load();
        for (unsigned i{}; i < 8; ++i)
            result.histogram[i] = v.histogram[i].load();
        for (unsigned i{}; i < 4; ++i)
            result.domainNs[i] = v.domainNs[i].load();
        return result;
    }
    struct Context {
        uint64_t previousResource{detail::resource};
        Domain previousDomain{detail::domain};
        Context(Domain domain, uint64_t resource) noexcept {
            detail::domain = domain;
            detail::resource = resource;
            if (auto t = detail::GetThread()) {
                t->resource.store(resource);
                t->domain.store(static_cast<unsigned>(domain));
            }
        }
        ~Context() {
            detail::domain = previousDomain;
            detail::resource = previousResource;
            if (auto t = detail::thread) {
                t->resource.store(previousResource);
                t->domain.store(static_cast<unsigned>(previousDomain));
            }
        }
    };
    struct CycleContext {
        uint64_t previous{detail::cycle};
        explicit CycleContext(uint64_t cycle) noexcept { detail::cycle = cycle; }
        ~CycleContext() { detail::cycle = previous; }
    };
    struct Scope {
        Metric metric;
        uint64_t start{}, bytes{}, resource{}, cycle{};
        Domain domain{};
        bool active{}, outer{};
        Scope(Metric metric, uint64_t bytes = 0) noexcept : metric(metric), start(Now()), bytes(bytes), active(true) {
            const auto depth = detail::depth[static_cast<unsigned>(metric)]++;
            outer = depth == 0 ||
                    (metric != Metric::FenceWait && metric != Metric::SubmitWait && metric != Metric::TrapDispatch);
            resource = detail::resource;
            cycle = detail::cycle;
            domain = detail::domain;
            if (auto t = detail::GetThread(); t && outer && metric == Metric::TrapDispatch)
                t->trapStart.store(start, std::memory_order_release);
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        void End() noexcept {
            if (!active)
                return;
            active = false;
            --detail::depth[static_cast<unsigned>(metric)];
            if (!outer)
                return; // Same metric nested on one thread is counted once.
            const auto elapsed = Now() - start;
            RecordAs(metric, elapsed, bytes, domain);
            if (auto t = detail::thread; t && metric == Metric::TrapDispatch) {
                t->trapNs.fetch_add(elapsed);
                t->trapStart.store(0, std::memory_order_release);
            }
            if (elapsed < 1000000)
                return;
            const auto seq = detail::sampleSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            auto &slot = detail::samples[seq % detail::samples.size()];
            if (slot.busy.test_and_set(std::memory_order_acquire)) {
                detail::sampleDrops.fetch_add(1);
                return;
            }
            slot.sample = {seq,   start,  elapsed, resource, bytes, detail::thread ? detail::thread->tid.load() : 0,
                           cycle, metric, domain};
            slot.busy.clear(std::memory_order_release);
        }
        ~Scope() { End(); }
    };
    inline std::array<std::atomic<uint64_t>, 9>
        faultCounts{}; // guest,host,eligible-read,eligible-write,unknown,accerr,maperr,no-esr,rejected-esr
    inline std::atomic<uint64_t> lastEsr{}, lastCode{};
    inline void ObserveFault(int signal, int code, std::span<const std::byte> records, bool host) noexcept {
        faultCounts[host ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
        const auto access = fault::Classify(records);
        const bool known = signal == SIGSEGV && code == SEGV_ACCERR;
        faultCounts[known && access == fault::Access::Read    ? 2
                    : known && access == fault::Access::Write ? 3
                                                              : 4]
            .fetch_add(1, std::memory_order_relaxed);
        if (code == SEGV_ACCERR)
            faultCounts[5].fetch_add(1, std::memory_order_relaxed);
        if (code == SEGV_MAPERR)
            faultCounts[6].fetch_add(1, std::memory_order_relaxed);
        lastCode.store(code, std::memory_order_relaxed);
        bool sawEsr{};
        // Preserve one raw syndrome for interpretation, even when the oracle rejects it.
        for (size_t offset{}; records.size() - offset >= 8;) {
            uint32_t magic{}, size{};
            std::memcpy(&magic, records.data() + offset, 4);
            std::memcpy(&size, records.data() + offset + 4, 4);
            if (!magic || size < 16 || size % 16 || size > records.size() - offset)
                break;
            if (magic == 0x45535201 && size == 16) {
                uint64_t esr{};
                std::memcpy(&esr, records.data() + offset + 8, 8);
                lastEsr.store(esr, std::memory_order_relaxed);
                sawEsr = true;
                break;
            }
            offset += size;
        }
        if (!sawEsr)
            faultCounts[7].fetch_add(1, std::memory_order_relaxed);
        else if (access == fault::Access::Unknown)
            faultCounts[8].fetch_add(1, std::memory_order_relaxed);
    }
    inline void Frame(bool queued, uint64_t timestamp = 0) noexcept {
        const auto now = Now();
        auto last = (queued ? detail::frameQueued : detail::framePresented).exchange(now);
        if (last)
            Record(queued ? Metric::FrameQueued : Metric::FramePresented, now - last);
        if (queued) {
            auto previous = detail::guestTimestamp.exchange(timestamp);
            if (previous && timestamp > previous)
                Record(Metric::GuestTimestampStep, timestamp - previous);
        }
    }
#else
    inline void Record(Metric, uint64_t = 0, uint64_t = 0) noexcept {}
    inline Value Read(Metric) noexcept { return {}; }
    struct Context {
        Context(Domain, uint64_t) noexcept {}
    };
    struct CycleContext {
        explicit CycleContext(uint64_t) noexcept {}
    };
    struct Scope {
        Scope(Metric, uint64_t = 0) noexcept {}
        void End() noexcept {}
    };
    inline void Frame(bool, uint64_t = 0) noexcept {}
#endif
} // namespace skyline::readback_probe
