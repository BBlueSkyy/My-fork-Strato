// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include "policy.inc"
using namespace skyline::gpu::interconnect::dynamic_descriptor_detail;
static void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
    Check(Capacity(0) == 1 && Capacity(1) == 1 && Capacity(3) == 4 && Capacity(4095) == 4096, "capacity rounding");
    Check(Fits({1, 2, 0}, {1, 4, 1}) && !Fits({1, 5, 0}, {1, 4, 1}), "layout growth detection");
    auto ranges = ChangedRanges(std::vector<unsigned>{1, 2, 3, 4}, std::vector<unsigned>{1, 8, 9, 4, 5});
    Check(ranges.size() == 2 && ranges[0].first == 1 && ranges[0].second == 2 && ranges[1].first == 4 && ranges[1].second == 1, "delta ranges");
    Check(ChangedRanges(std::vector<unsigned>{1, 2}, std::vector<unsigned>{1, 2}).empty(), "unchanged descriptors rewritten");
    Check(ChooseSnapshot(true, true, false) == SnapshotAction::Reuse, "same execution must reuse set");
    Check(ChooseSnapshot(true, false, false) == SnapshotAction::Clone, "pending snapshot must not mutate");
    Check(ChooseSnapshot(true, false, true) == SnapshotAction::ResetAndReuse, "retired snapshot should recycle");
    Check(ChooseSnapshot(false, true, false) == SnapshotAction::Clone, "changed same-execution snapshot must stay immutable");
    Check(Encode(3, 0) == 12 && Encode(3, 1) == 13 && Encode(3, 2) == 14, "numeric table tags");
    Check(SamplerIndex(0xFEDABCDEu, false) == 0xFED && SamplerIndex(0xFEDABCDEu, true) == 0xABCDE, "header sampler binding");
    std::cout << "PASS dynamic descriptor capacity, incremental updates, immutable snapshot lifecycle, numeric and sampler ABI\n";
}
