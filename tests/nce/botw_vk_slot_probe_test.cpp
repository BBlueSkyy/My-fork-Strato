// SPDX-License-Identifier: MPL-2.0
#include "../../app/src/main/cpp/skyline/common/botw_vk_slot_probe.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace skyline::diag;
    assert(vkSlotSampleState.load() == VkSlotSampleState::Idle);
    assert(ArmVkSlotSample());
    assert(!ArmVkSlotSample());
    assert(BeginVkSlotSample());
    assert(!BeginVkSlotSample());
    assert(vkSlotSampleActive);

    for (unsigned i{}; i < VkSampleMaxDraws + 10; ++i)
        assert(RecordVkSampleDraw() == (i < VkSampleMaxDraws));
    for (unsigned i{}; i < VkSampleMaxPasses + 10; ++i)
        assert(RecordVkSamplePass() == (i < VkSampleMaxPasses));
    RecordVkSampleSubpass();
    RecordVkSampleSubpass();

    assert(vkSlotRecordedDraws == VkSampleMaxDraws + 10);
    assert(vkSlotRecordedPasses == VkSampleMaxPasses + 10);
    assert(vkSlotRecordedSubpasses == 2);
    CompleteVkSlotSample();
    assert(!vkSlotSampleActive);
    assert(vkSlotSampleState.load() == VkSlotSampleState::Submitted);
    assert(!RecordVkSampleDraw());
    assert(!RecordVkSamplePass());
    assert(!ArmVkSlotSample());
    assert(!BeginVkSlotSample());
    std::puts("PASS: BOTW one-shot Vulkan recording-slot sample bounds");
}
