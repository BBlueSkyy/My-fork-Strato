#include "gpu/texture/lifecycle_trace.h"
#include <cassert>
#include <sstream>
#include <thread>
#include <fstream>
using namespace skyline::gpu::texture::diagnostic;
int main(int argc, char **argv) {
    assert(UsesIdentityMapping(Event::StorageRead));
    assert(UsesIdentityMapping(Event::StorageWriteIntent));
    assert(!UsesIdentityMapping(Event::SampleRead));
    Recorder<8> trace;
    Token token;
    Snapshot state{};
    state.id = 7;
    assert(!trace.Record(token, Event::SampleRead, state));
    trace.Start(true);
    assert(trace.Record(token, Event::SampleRead, state));
    assert(trace.Record(token, Event::SampleRead, state));
    assert(!trace.Record(token, Event::SampleRead, state));
    assert(trace.SetPhase(1));
    assert(trace.Record(token, Event::SampleRead, state));
    assert(!trace.SetPhase(5));
    state.state = 2;
    state.writeSerial = 0;
    assert(trace.Record(token, Event::InlineGpuOwnership, state));
    std::ostringstream csv;
    trace.Export(csv);
    assert(csv.str().find("InlineGpuOwnership") != std::string::npos);
    assert(csv.str().find("limited=1") != std::string::npos);
    assert(csv.str().find("phase=1") != std::string::npos);
    if (argc > 1) { std::ofstream out{argv[1]}; out << csv.str(); }
    trace.Frame();
    assert(trace.SetPhase(1));
    assert(!trace.SetPhase(0));
    std::ostringstream markers; trace.Export(markers);
    assert(markers.str().find("marker_frame=1") == std::string::npos);
    for (int i=0; i<20; ++i) { Token t; trace.Record(t, Event::Create, state); }
    assert(trace.Count() == 10);
    csv.str(""); trace.Export(csv);
    assert(csv.str().find("phase=1 contention=0 overflow=14") != std::string::npos);
    trace.Start(false);
    assert(!trace.Record(token, Event::FreeGuest, state));
    assert(trace.Count() == 0);
    trace.Start(true);
    Token shared;
    std::thread a([&] { for (int i=0;i<1000;++i) trace.Record(shared,Event::SampleRead,state); });
    std::thread b([&] { for (int i=0;i<1000;++i) trace.Record(shared,Event::FreeGuest,state); });
    a.join(); b.join();
    assert(trace.Count() <= 4);
    trace.Freeze();
    auto count = trace.Count();
    assert(!trace.Record(shared, Event::Create, state));
    assert(!trace.SetPhase(2));
    std::ostringstream first, second;
    trace.Export(first); trace.Export(second);
    assert(first.str() == second.str());
    assert(count == trace.Count());
}
