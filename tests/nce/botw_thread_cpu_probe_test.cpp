// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/common/botw_thread_cpu_probe.h"
#include <cassert>
#include <cstdio>
#include <string>

int main() {
    std::string stat{"1234 (HOS-7) S"};
    for (int field = 4; field <= 13; ++field)
        stat += " 0";
    stat += " 125 75 0 0";

    auto task{skyline::diag::ParseBotwTaskStat(1234, stat)};
    assert(task.has_value());
    assert(task->tid == 1234);
    assert(task->name == "HOS-7");
    assert(task->state == 'S');
    assert(task->ticks == 200);

    // Linux task names may contain spaces or a literal right parenthesis.
    stat.replace(stat.find("HOS-7"), 5, "HOS-) worker");
    task = skyline::diag::ParseBotwTaskStat(99, stat);
    assert(task && task->name == "HOS-) worker" && task->ticks == 200);

    assert(!skyline::diag::ParseBotwTaskStat(1, "malformed"));
    assert(!skyline::diag::ParseBotwTaskStat(1, "1 (HOS-1) S 2 3"));

    // No special permissions or signal delivery required to inspect our
    // own process; missing /proc will produce a readable diagnostic.
    skyline::diag::BotwThreadCpuSampler sampler;
    const auto message{sampler.Snapshot()};
    assert(!message.empty());
    std::puts("PASS: BOTW guest CPU sampler parses Linux task stats");
}
