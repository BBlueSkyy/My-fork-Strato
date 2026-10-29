#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>
#include <fmt/format.h>
std::vector<std::string> records;
#define LOGI(...) records.push_back(fmt::format(__VA_ARGS__))
#define LOGE(...) records.push_back(fmt::format(__VA_ARGS__))
#include <loader/startup_trace.h>
int main() {
    { skyline::loader::StartupTrace trace("success"); trace.Complete(); trace.Complete(); }
    assert(records.size() == 2 && records[0].find("enter") != std::string::npos && records[1].find("ok") != std::string::npos);
    records.clear();
    try { skyline::loader::StartupTrace trace("failure"); throw std::runtime_error("synthetic failure"); } catch (...) {}
    assert(records.size() == 2 && records.back().find("unwind") != std::string::npos && records.back().find("us=") != std::string::npos);
    records.clear();
    { skyline::loader::StartupTrace trace("unfinished"); }
    assert(records.size() == 2 && records.back().find("incomplete") != std::string::npos);
}
