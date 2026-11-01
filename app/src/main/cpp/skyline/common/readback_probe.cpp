// SPDX-License-Identifier: MPL-2.0
#include "readback_probe_session.h"
#if STRATO_READBACK_DIAGNOSTICS
#include <algorithm>
#include <functional>
#include <pthread.h>
#include <fstream>
#include <sstream>
#include <logger/logger.h>
#endif
namespace skyline::readback_probe {
#if STRATO_READBACK_DIAGNOSTICS
    namespace {
        template <class... Args> void Report(fmt::format_string<Args...> format, Args &&...args) {
            if (AsyncLogger::CheckLogLevel(AsyncLogger::LogLevel::Info))
                AsyncLogger::LogSync(AsyncLogger::LogLevel::Info, fmt::format(format, std::forward<Args>(args)...),
                                     nullptr);
        }
        uint64_t Frequency() {
            for (const char *path : {"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/class/kgsl/kgsl-3d0/gpuclk"}) {
                uint64_t value{};
                std::ifstream input(path);
                if (input >> value)
                    return value;
            }
            return 0;
        }
    } // namespace
    Session::Session(bool fast, bool writes, bool clocks, bool freeGuest) {
        try {
            const auto begin = Now();
            std::array<Value, Names.size()> initial{};
            for (unsigned i{}; i < Names.size(); ++i)
                initial[i] = Read(static_cast<Metric>(i));
            std::array<uint64_t, 9> initialFaults{};
            for (unsigned i{}; i < 9; ++i)
                initialFaults[i] = faultCounts[i].load();
            const auto initialSequence = detail::sampleSequence.load();
            std::array<uint64_t, 64> initialThreadNs{};
            for (unsigned i{}; i < 64; ++i)
                initialThreadNs[i] = detail::threads[i].trapNs.load();
            reporter = std::thread([this, fast, writes, clocks, freeGuest, begin, initial, initialFaults,
                                    initialSequence, initialThreadNs] {
                try {
                    pthread_setname_np(pthread_self(), "Sky-ReadbackDiag");
                    AsyncLogger::UpdateTag();
                    uint64_t previous = begin, previousSequence = initialSequence;
                    auto before = initial;
                    auto previousFaults = initialFaults;
                    auto previousThreadNs = initialThreadNs;
                    Report("ReadbackProbe v1 start={} baseline=master routing=legacy-write observer=PR412 fast={} "
                           "fast_writes={} max_clocks={} free_guest={} interval_ms=5000",
                           begin, fast, writes, clocks, freeGuest);
                    auto report = [&] {
                        const auto now = Now();
                        Report("ReadbackProbe window start={} end={} wall_ns={} gpu_hz_point={} (0=unavailable) "
                               "ns=completed-thread-spans/inclusive-not-frame-time",
                               previous, now, now - previous, Frequency());
                        previous = now;
                        for (unsigned group{}; group < 3; ++group) {
                            std::ostringstream text;
                            for (unsigned i = group; i < Names.size(); i += 3) {
                                const auto v = Read(static_cast<Metric>(i));
                                const auto &old = before[i];
                                if (v.calls != old.calls || v.bytes != old.bytes || v.ns != old.ns ||
                                    v.max != old.max || v.histogram != old.histogram || v.domainNs != old.domainNs) {
                                    text << Names[i] << "=" << v.calls - old.calls << "," << v.bytes - old.bytes << ","
                                         << v.ns - old.ns << ",max_all:" << v.max << ",h:";
                                    for (unsigned b{}; b < 8; ++b)
                                        text << (b ? "/" : "") << v.histogram[b] - old.histogram[b];
                                    text << ",kind_ns:";
                                    for (unsigned d{}; d < 4; ++d)
                                        text << (d ? "/" : "") << v.domainNs[d] - old.domainNs[d];
                                    text << " ";
                                }
                                before[i] = v;
                            }
                            Report("ReadbackProbe metrics{} "
                                   "calls,bytes,ns,max_all_ns,h_us[1/10/100/1000/4000/16000/64000/inf]: {}",
                                   group, text.str());
                        }
                        std::ostringstream faults;
                        for (unsigned i{}; i < 9; ++i) {
                            auto value = faultCounts[i].load();
                            faults << (i ? "," : "") << value - previousFaults[i];
                            previousFaults[i] = value;
                        }
                        Report("ReadbackProbe nce "
                               "guest,host,would412read,known_write,fallback_write_unknown,accerr,maperr,no_esr,rejected_esr={} "
                               "last_seen_esr={:#x} last_signal_code={} (observed; NOT routing changed)",
                               faults.str(), lastEsr.load(), lastCode.load());
                        std::array<std::pair<uint64_t, uint64_t>, 64> threads{};
                        std::ostringstream active;
                        for (unsigned i{}; i < 64; ++i) {
                            auto &t = detail::threads[i];
                            const auto ns = t.trapNs.load();
                            threads[i] = {ns - previousThreadNs[i], t.tid.load()};
                            previousThreadNs[i] = ns;
                            const auto start = t.trapStart.load(std::memory_order_acquire);
                            if (start && now >= start)
                                active << t.tid.load() << ":" << now - start << "ns ";
                        }
                        std::sort(threads.begin(), threads.end(), std::greater<>());
                        Report("ReadbackProbe trap_threads tid:completed_ns={}:{},{}:{},{}:{} active_tid:age={} "
                               "overflow={}",
                               threads[0].second, threads[0].first, threads[1].second, threads[1].first,
                               threads[2].second, threads[2].first, active.str(), detail::threadOverflow.load());
                        std::array<Sample, 128> samples{};
                        unsigned used{};
                        const auto sequence = detail::sampleSequence.load();
                        for (auto &slot : detail::samples) {
                            if (slot.busy.test_and_set(std::memory_order_acquire))
                                continue;
                            const auto sample = slot.sample;
                            slot.busy.clear(std::memory_order_release);
                            if (sample.sequence > previousSequence && sample.sequence <= sequence)
                                samples[used++] = sample;
                        }
                        std::sort(samples.begin(), samples.begin() + used,
                                  [](const auto &a, const auto &b) { return a.ns > b.ns; });
                        for (unsigned i{}; i < std::min(used, 3U); ++i) {
                            const auto &v = samples[i];
                            Report("ReadbackProbe slow_recent metric={} tid={} start={} ns={} kind={} resource={:#x} "
                                   "bytes={} seq={} cycle_ptr={:#x}",
                                   Names[unsigned(v.metric)], v.tid, v.start, v.ns, unsigned(v.domain), v.resource,
                                   v.bytes, v.sequence, v.cycle);
                        }
                        Report(
                            "ReadbackProbe slow_coverage produced={} retained={} missing_at_least={} busy_drops_all={}",
                            sequence - previousSequence, used,
                            sequence - previousSequence > 128 ? sequence - previousSequence - 128 : 0,
                            detail::sampleDrops.load());
                        previousSequence = sequence;
                    };
                    std::unique_lock lock{mutex};
                    while (!condition.wait_for(lock, std::chrono::seconds(5), [this] { return stopping; })) {
                        lock.unlock();
                        report();
                        lock.lock();
                    }
                    lock.unlock();
                    report();
                } catch (...) {
                } // A diagnostic failure must not kill emulation.
            });
        } catch (...) {
        } // Reporter creation is optional.
    }
    void Session::Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        condition.notify_all();
        if (reporter.joinable())
            reporter.join();
    }
    Session::~Session() { Stop(); }
#else
    Session::Session(bool, bool, bool, bool) {}
    void Session::Stop() {}
    Session::~Session() = default;
#endif
} // namespace skyline::readback_probe
