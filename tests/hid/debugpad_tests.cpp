// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <cstring>
#include <fstream>
#include <thread>
#include <skyline/input/debugpad.h>

using namespace skyline;
using namespace skyline::input;

static void Check(const DebugPadSection &section, u64 sequence) {
    assert(section.header.entryCount == constant::HidEntryCount);
    assert(section.header.maxEntry == std::min<u64>(sequence, constant::HidEntryCount - 1));
    const auto &entry{section.entries[section.header.currentEntry]};
    assert(entry.globalTimestamp == sequence * 2);
    assert(entry.localTimestamp == sequence);
    assert(entry.attributes == 0 && entry.buttons == 0);
    assert(entry.rightX == 0 && entry.rightY == 0 && entry.leftX == 0 && entry.leftY == 0);
}

int main(int argc, char **argv) {
    DebugPadSection section{};
    section.header.entryCount = constant::HidEntryCount;
    DebugPadManager manager{section};
    manager.UpdateSharedMemory(10);
    assert(section.header.maxEntry == 0); // Inactive ring remains empty.
    manager.Activate(20);
    Check(section, 1); // Singular SDK reads are safe immediately after activation.
    assert(section.header.timestamp == 20);
    manager.Activate(21);
    Check(section, 1); // Activation is idempotent and does not reset history.

    for (u64 sequence{2}; sequence <= 100; ++sequence) {
        manager.UpdateSharedMemory(sequence + 20);
        Check(section, sequence);
        const auto &previous{section.entries[(section.header.currentEntry + 16) % 17]};
        assert(section.entries[section.header.currentEntry].localTimestamp - previous.localTimestamp == 1);
    }
    const auto update = [&] { for (unsigned i{}; i < 100; ++i) manager.UpdateSharedMemory(200); };
    std::thread first{update}, second{update};
    first.join();
    second.join();
    Check(section, 300);
    manager.Activate(201);
    Check(section, 300);
    for (const auto &entry : section.entries) {
        assert(entry.attributes == 0 && entry.buttons == 0);
        assert(entry.leftX == 0 && entry.leftY == 0 && entry.rightX == 0 && entry.rightY == 0);
    }
    if (argc == 2) { // Optional private SDK replay consumes the actual production bytes.
        std::ofstream output{argv[1], std::ios::binary};
        output.write(reinterpret_cast<const char *>(&section), sizeof(section));
        assert(output.good());
    }
}
