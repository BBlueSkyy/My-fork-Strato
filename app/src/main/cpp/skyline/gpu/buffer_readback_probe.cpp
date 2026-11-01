// SPDX-License-Identifier: MPL-2.0
#include "buffer_readback_probe.h"
#if STRATO_BUFFER_READBACK_PROBE
#include <algorithm>
#include <functional>
#include <sstream>
#include <logger/logger.h>
namespace skyline::gpu::buffer_readback_probe {
    namespace {
        template <class... Args> void Log(fmt::format_string<Args...> format, Args &&...args) {
            if (AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Info))
                AsyncLogger::LogSync(AsyncLogger::LogLevel::Info, fmt::format(format, std::forward<Args>(args)...),
                                     nullptr);
        }
        Value Delta(Value a, Value b) { return {a.calls - b.calls, a.bytes - b.bytes, a.ns - b.ns}; }
        using Row = std::array<Value, Count>;
        Row ReadRow(const std::array<Counter, Count> &counters) {
            Row row;
            for (unsigned p{}; p < Count; ++p)
                row[p] = Read(counters[p]);
            return row;
        }
        std::string Print(const Row &row) {
            std::ostringstream out;
            for (unsigned p{}; p < Count; ++p)
                out << Names[p] << "=" << row[p].calls << "," << row[p].bytes << "," << row[p].ns << " ";
            return out.str();
        }
    } // namespace
    struct Session::State {
        std::mutex mutex;
        std::condition_variable condition;
        bool stopping{};
        std::thread thread;
        Row beforeTotal{};
        std::array<Row, 512> before{};
        uint64_t previous{Now()};
        State() {
            beforeTotal = ReadRow(totals);
            for (unsigned i{}; i < 512; ++i)
                before[i] = ReadRow(entries[i].phases);
        }
        void Report() {
            const auto now = Now();
            Row total;
            for (unsigned p{}; p < Count; ++p) {
                auto v = Read(totals[p]);
                total[p] = Delta(v, beforeTotal[p]);
                beforeTotal[p] = v;
            }
            Log("BufferReadback v2 window start={} end={} wall_ns={} calls,bytes,completed_thread_ns (inclusive,not "
                "frame time): {}",
                previous, now, now - previous, Print(total));
            previous = now;
            // Heap-owned snapshots keep the reporter's native stack bounded.
            struct Hot {
                uint64_t rank{}, id{}, size{};
                Row delta{};
            };
            std::array<Hot, 4> hot{};
            unsigned active{};
            for (unsigned i{}; i < 512; ++i) {
                auto key = entries[i].key.load();
                if (!key)
                    continue;
                Row delta;
                bool changed{};
                for (unsigned p{}; p < Count; ++p) {
                    auto v = Read(entries[i].phases[p]);
                    changed |= v != before[i][p];
                    delta[p] = Delta(v, before[i][p]);
                    before[i][p] = v;
                }
                if (!changed)
                    continue;
                ++active;
                auto rank = delta[DirtyPrepare].ns + delta[ReadbackWait].ns;
                Hot next{rank, key - 1, entries[i].size.load(), delta};
                if (rank > hot.back().rank) {
                    hot.back() = next;
                    std::sort(hot.begin(), hot.end(), [](const auto &a, const auto &b) { return a.rank > b.rank; });
                }
            }
            for (const auto &row : hot)
                if (row.rank)
                    Log("BufferReadback v2 hot id={} size={} rank_gpu_dirty_thread_ns={} {}", row.id, row.size,
                        row.rank, Print(row.delta));
            Log("BufferReadback v2 coverage active_ids={} top_dirty_ids_only=4 dropped_records_all={} (restart app per "
                "capture; IDs belong to one BufferManager)",
                active, dropped.load());
        }
    };
    Session::Session(bool fast, bool writes, bool clocks, bool freeGuest) {
        try {
            state = std::make_unique<State>();
            auto p = state.get();
            p->thread = std::thread([p, fast, writes, clocks, freeGuest] {
                try {
                    AsyncLogger::UpdateTag();
                    Log("BufferReadback v2 start fast={} fast_writes={} max_clocks={} free_guest={} capacity=512 "
                        "interval_ms=5000",
                        fast, writes, clocks, freeGuest);
                    std::unique_lock lock{p->mutex};
                    while (!p->condition.wait_for(lock, std::chrono::seconds(5), [p] { return p->stopping; })) {
                        lock.unlock();
                        p->Report();
                        lock.lock();
                    }
                    lock.unlock();
                    p->Report();
                } catch (...) {
                }
            });
        } catch (...) {
        }
    }
    void Session::Stop() {
        if (!state)
            return;
        {
            std::scoped_lock lock{state->mutex};
            state->stopping = true;
        }
        state->condition.notify_all();
        if (state->thread.joinable())
            state->thread.join();
    }
    Session::~Session() { Stop(); }
} // namespace skyline::gpu::buffer_readback_probe
#else
namespace skyline::gpu::buffer_readback_probe {
    Session::Session(bool, bool, bool, bool) {}
    void Session::Stop() {}
    Session::~Session() = default;
} // namespace skyline::gpu::buffer_readback_probe
#endif
