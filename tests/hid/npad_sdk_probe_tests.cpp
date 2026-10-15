// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <cstring>
#include <sys/mman.h>
#include <skyline/input/npad_sdk_probe.h>
#include <skyline/hle/probe_read.h>

int main() {
    using namespace skyline::input::diagnostic;
    auto full{IdentifyReader("nn::hid::GetNpadStates(nn::hid::NpadFullKeyState*, int, unsigned int const&)")};
    assert(full && full->layout == 0 && full->plural);
    auto handheld{IdentifyReader("nn::hid::GetNpadState(nn::hid::NpadHandheldState*, unsigned int const&)")};
    assert(handheld && handheld->layout == 1 && !handheld->plural);
    auto dual{IdentifyReader("nn::hid::GetNpadStates(nn::hid::NpadJoyDualState*, int, unsigned int const&)")};
    assert(dual && dual->layout == 2);
    auto ext{IdentifyReader("nn::hid::system::GetNpadState(nn::hid::system::NpadSystemExtState*, unsigned int const&)")};
    assert(ext && ext->layout == 6 && !ext->plural);
    assert(!IdentifyReader("nn::hid::GetNpadState(nn::hid::NpadFullKeyState*, unsigned int)"));
    assert(!IdentifyReader("nn::hid::GetNpadStates(nn::hid::NpadFullKeyState*, long, unsigned int const&)"));
    assert(!IdentifyReader("nn::hid::GetBasicXpadStates(nn::hid::BasicXpadState*, int, unsigned int const&)"));
    assert(NpadSlot(0) == 0 && NpadSlot(7) == 7 && NpadSlot(0x20) == 8 && NpadSlot(0x10) == 9);
    assert(!NpadSlot(8) && !NpadSlot(0x21));
    NpadOutput neutral{.samplingNumber = 123, .buttons = 0, .lx = 0, .ly = 0,
        .rx = 0, .ry = 0, .attributes = 3, .reserved = 0};
    auto different{neutral};
    different.lx = -32768;
    assert(different != neutral);
    different = neutral;
    different.samplingNumber++;
    assert(different != neutral); // Equal neutral payloads do not identify the same sample.

    auto *page{static_cast<unsigned char *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))};
    assert(page != MAP_FAILED);
    std::memcpy(page, &neutral, sizeof(neutral));
    NpadOutput copied{};
    assert(skyline::hle::ReadProbeMemory(&copied, page, sizeof(copied)));
    assert(copied == neutral);
    assert(mprotect(page, 4096, PROT_NONE) == 0);
    assert(!skyline::hle::ReadProbeMemory(&copied, page, sizeof(copied)));
    assert(munmap(page, 4096) == 0);
    assert(!skyline::hle::ReadProbeMemory(&copied, page, sizeof(copied)));
}
