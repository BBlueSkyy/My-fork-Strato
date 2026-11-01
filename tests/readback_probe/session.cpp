#include <common/readback_probe_session.h>
#include <logger/logger.h>
#include <cassert>
#include <iostream>
#include <mutex>
#include <vector>
#include <string_view>
std::mutex logMutex;
std::vector<std::string> logs;
std::vector<const char *> queuedTags;
bool deferred{}, sinkAlive{true};
thread_local std::string loggerTag(128, 'd');
namespace skyline {
    void AsyncLogger::UpdateTag() { (void)loggerTag.size(); }
    bool AsyncLogger::CheckLogLevel(LogLevel) {
        assert(sinkAlive);
        return true;
    }
    void AsyncLogger::LogAsync(LogLevel, std::string &&text, const char *) {
        std::scoped_lock lock{logMutex};
        logs.push_back(std::move(text));
        if (deferred)
            queuedTags.push_back(loggerTag.data()); // Same TLS ownership as production logger.
    }
    void AsyncLogger::LogSync(LogLevel, std::string &&text, const char *) {
        assert(loggerTag[0] == 'd');
        std::scoped_lock lock{logMutex};
        logs.push_back(std::move(text));
    }
} // namespace skyline
using namespace skyline::readback_probe;
int main(int argc, char **argv) {
    const auto mode = argc > 1 ? std::string_view(argv[1]) : "normal";
    if (mode == "lifetime") {
        deferred = true;
        {
            Session session(false, false, false, false);
            Record(Metric::BufferCopy, 1000, 4096);
        }
        for (auto tag : queuedTags) {
            volatile char value = tag[0];
            assert(value == 'd');
        }
        assert(queuedTags.empty());
        std::cout << "PASS reporter emits no queued references to exiting thread TLS\n";
        return 0;
    }
    if (mode == "late") {
        {
            Session session(false, false, false, false);
            // Valid snapshot interleaving: calls was captured before a Record's
            // remaining atomic fields. No new calls arrive in this window.
            detail::metrics[unsigned(Metric::BufferCopy)].bytes.fetch_add(31);
            detail::metrics[unsigned(Metric::BufferCopy)].ns.fetch_add(13);
        }
        bool found{};
        for (const auto &text : logs)
            found |= text.find("buf_memcpy=0,31,13") != std::string::npos;
        assert(found);
        std::cout << "PASS late atomic fields remain visible without new call count\n";
        return 0;
    }
    const auto begin = Now();
    {
        Session first(false, false, false, false);
        Record(Metric::BufferCopy, 1000, 4096);
    }
    {
        Session second(true, true, false, false);
        Record(Metric::BufferHack, 1000, 4096);
    }
    assert(Now() - begin < 1000000000ULL);
    unsigned starts{}, copies{}, hacks{};
    for (const auto &text : logs) {
        starts += text.find("ReadbackProbe v1 start=") != std::string::npos;
        copies += text.find("buf_memcpy=1,4096,1000") != std::string::npos;
        hacks += text.find("buf_hack=1,4096,1000") != std::string::npos;
    }
    assert(starts == 2 && copies == 1 && hacks == 1);
    {
        Session session(false, false, false, false);
        session.Stop();
        sinkAlive = false; // Models OS::Execute's logger finalization after Stop.
        session.Stop();    // Destructor will call Stop again; neither may log.
    }
    sinkAlive = true;
    std::cout << "PASS actual fmt/log schema, session deltas, final report and prompt stop\n";
}
