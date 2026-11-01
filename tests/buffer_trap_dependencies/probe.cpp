#include <gpu/buffer_readback_probe.h>
#include <logger/logger.h>
#include <cassert>
#include <future>
#include <string>
#include <vector>
#include <iostream>
namespace bp = skyline::gpu::buffer_readback_probe;
std::mutex logMutex;
std::vector<std::string> logs;
bool alive{true};
namespace skyline {
    void AsyncLogger::UpdateTag() {}
    bool AsyncLogger::CheckLogLevel(LogLevel) {
        assert(alive);
        return true;
    }
    void AsyncLogger::LogAsync(LogLevel, std::string &&, const char *) {
        assert(false && "queued reporter TLS must not escape");
    }
    void AsyncLogger::LogSync(LogLevel, std::string &&text, const char *) {
        std::scoped_lock lock{logMutex};
        logs.push_back(std::move(text));
    }
} // namespace skyline
int main() {
    for (uint64_t i = 1000; i < 3000; ++i)
        bp::Record(i, 4096, bp::Upload, 20, 4096);
    unsigned used{};
    for (auto &e : bp::entries)
        used += e.key.load() != 0;
    assert(!used); // Initial uploads cannot crowd out dirty resources.
    std::vector<std::future<void>> workers;
    for (unsigned i{}; i < 8; ++i)
        workers.push_back(std::async(std::launch::async, [] {
            for (unsigned n{}; n < 1000; ++n)
                bp::Record(81, 65536, bp::Download, 2000, 65536);
        }));
    for (auto &worker : workers)
        worker.get();
    auto e = bp::Find(81, 65536, false);
    assert(e);
    auto v = bp::Read(e->phases[bp::Download]);
    assert(v.calls == 8000 && v.bytes == 524288000 && v.ns == 16000000);
    unsigned samples{};
    for (unsigned i{}; i < 32; ++i)
        samples += bp::SampleWait(81, 65536);
    assert(samples == 2);
    {
        bp::Session session(false, false, false, false);
        bp::Record(81, 65536, bp::DirtyPrepare, 1000000);
        {
            bp::WaitTrace trace{81, 65536, true};
            {
                bp::FenceWaitScope root{&trace};
                root.Entry(false, false);
                root.Next(bp::Driver);
                { bp::FenceWaitScope child{&trace}; child.Entry(true, true); child.Next(bp::Tail); }
            }
            trace.Finish();
            assert(trace.depth == 0 && trace.nodes == 2 && trace.unsubmitted && !trace.alreadySignalled);
        }
        // A valid split-counter snapshot must emit late fields even with no new call.
        bp::totals[bp::Download].bytes.fetch_add(31);
        bp::totals[bp::Download].ns.fetch_add(13);
        session.Stop();
        alive = false;
        session.Stop();
    }
    alive = true;
    bool hot{}, late{}, phases{};
    for (const auto &text : logs) {
        hot |= text.find("hot id=81 size=65536") != std::string::npos;
        late |= text.find("download=0,31,13") != std::string::npos;
        phases |= text.find("BufferReadback v3 hot id=81 size=65536") != std::string::npos && text.find("unsubmitted=1,0,0") != std::string::npos;
    }
    assert(hot && late && phases);
    for (uint64_t i = 4000; i < 5000; ++i)
        bp::Record(i, 4096, bp::Download, 20, 4096);
    assert(bp::dropped.load() > 0);
    std::cout << "PASS buffer attribution, concurrent exact counters, non-dirty admission, bounded overflow, "
                 "hot-resource reporting, late atomic fields, synchronous logger and Stop-before-finalize\n";
}
