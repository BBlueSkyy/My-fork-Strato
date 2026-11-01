#pragma once
#include <ctime>
#include <future>
// Observe existing diagnostic clock calls, without adding a production test hook
// or replacing the actual monotonic time. Each test selects a root phase entry.
namespace trace_test {
inline thread_local unsigned clockCalls{}, notifyAt{};
inline thread_local std::promise<void> *entered{};
template<class Cycle, class Trace>
void Wait(Cycle &cycle, Trace &trace, std::promise<void> &entry, unsigned call) {
    clockCalls=0;notifyAt=call;entered=&entry;
    try { cycle.Wait(false,&trace); }
    catch (...) { entered=nullptr;throw; }
    entered=nullptr;
}
}
extern "C" int __real_clock_gettime(clockid_t, timespec *);
extern "C" int __wrap_clock_gettime(clockid_t clock, timespec *value) {
    const int result=__real_clock_gettime(clock,value);
    if (trace_test::entered && ++trace_test::clockCalls==trace_test::notifyAt)
        trace_test::entered->set_value();
    return result;
}
