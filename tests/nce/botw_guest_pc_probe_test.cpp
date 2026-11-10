// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/common/botw_guest_pc_probe.h"
#include <cassert>
#include <cstdint>
#include <string>

int main() {
    using namespace skyline::diag;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    ConfigureBotwProbes(BotwProgramId);

    BotwGuestPcSampler sampler;
    const std::string empty{sampler.Snapshot()};
    assert(empty.find("passiveGuestSignalSamples=0") != std::string::npos);

    RecordBotwGuestPc(1, 0x800100000, 0x800100050, 0x1900100000);
    RecordBotwGuestPc(1, 0x800100000, 0x800100050, 0x1900100000);
    RecordBotwGuestPc(1, 0x800100004, 0x800100054, 0x1900100000);
    RecordBotwGuestPc(4, 0x800200000, 0x800200040, 0x1900200000);
    RecordBotwGuestPc(MaxTrackedGuestThreads, 0xBAD, 0, 0); // Must be ignored.

    const std::string first{sampler.Snapshot()};
    assert(first.find("passiveGuestSignalSamples=4") != std::string::npos);
    assert(first.find("HOS-1{samples=3,samePc=1,pc=0x800100004") != std::string::npos);
    assert(first.find("HOS-4{samples=1,samePc=0,pc=0x800200000") != std::string::npos);

    const std::string second{sampler.Snapshot()};
    assert(second.find("passiveGuestSignalSamples=0") != std::string::npos);

    RecordBotwGuestPc(1, 0x800100004, 0x800100054, 0x1900100000);
    const std::string third{sampler.Snapshot()};
    assert(third.find("passiveGuestSignalSamples=1") != std::string::npos);
    assert(third.find("HOS-1{samples=1,samePc=1") != std::string::npos);
}
