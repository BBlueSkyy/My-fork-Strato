#include "readback_diagnostics.h"
#include <cassert>
#include <thread>
#include <vector>
using namespace skyline::gpu::readback_diagnostics;
int main() {
    Counters c;
    for (bool enabled : {false,true})
        for (bool threshold : {false,true}) for (bool alias : {false,true})
            for (bool freed : {false,true}) {
                auto before=c.Snapshot();
                c.Decision(enabled,threshold,alias,freed);
                auto after=c.Snapshot();
                auto expected=!enabled ? Disabled : !threshold ? BelowThreshold : alias ? Aliased : freed ? Freed : FastEligible;
                for (size_t i=Disabled;i<=FastEligible;i++) assert(after[i]-before[i]==(i==expected));
                assert(after[FastTaken]==0 && after[FastWrites]==0);
            }
    std::vector<std::thread> threads;
    for (int i=0;i<4;i++) threads.emplace_back([&]{for(int j=0;j<10000;j++) {c.Add(ReadTrap);c.Wait(17);}});
    for(auto &t:threads)t.join();
    auto s=c.Snapshot();assert(s[ReadTrap]==40000 && s[WaitCount]==40000 && s[WaitNs]==680000);
    Reporter r;
    assert(r.Due(100)); assert(!r.Due(100)); assert(!r.Due(5000000099));
    assert(r.Due(5000000100)); assert(!r.Due(5000000101));
}
