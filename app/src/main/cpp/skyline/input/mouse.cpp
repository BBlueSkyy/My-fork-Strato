// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "mouse.h"

namespace skyline::input {
    MouseManager::MouseManager(HidSharedMemory *hid) : section{hid->mouse} {}

    void MouseManager::Activate() {
        std::scoped_lock lock{mutex};
        activated = true;
    }

    void MouseManager::SetState(i32 x, i32 y, i32 deltaX, i32 deltaY, i32 wheelX, i32 wheelY, u32 buttons) {
        std::scoped_lock lock{mutex};
        state.positionX = std::max(x, 0);
        state.positionY = std::max(y, 0);
        state.deltaX += deltaX;
        state.deltaY += deltaY;
        state.scrollChangeX += wheelX;
        state.scrollChangeY += wheelY;
        state.buttons.raw = buttons & 0x1F;
        state.attributes.connected = true;
    }

    void MouseManager::UpdateSharedMemory() {
        std::scoped_lock lock{mutex};
        if (!activated)
            return;

        const auto &lastEntry{section.entries[section.header.currentEntry]};
        section.header.timestamp = util::GetTimeTicks();
        section.header.entryCount = constant::HidEntryCount;
        section.header.maxEntry = std::min<u64>(section.header.maxEntry + 1, constant::HidEntryCount - 1);
        section.header.currentEntry = (section.header.currentEntry + 1) % constant::HidEntryCount;

        auto &entry{section.entries[section.header.currentEntry]};
        const auto nextSamplingNumber{lastEntry.localTimestamp + 1};
        const auto completedMarker{nextSamplingNumber << 1};
        __atomic_store_n(&entry.globalTimestamp, completedMarker | 1, __ATOMIC_RELAXED);
        entry.localTimestamp = nextSamplingNumber;
        entry.positionX = state.positionX;
        entry.positionY = state.positionY;
        entry.deltaX = state.deltaX;
        entry.deltaY = state.deltaY;
        entry.scrollChangeY = state.scrollChangeY;
        entry.scrollChangeX = state.scrollChangeX;
        entry.buttons = state.buttons;
        entry.attributes = state.attributes;
        __atomic_store_n(&entry.globalTimestamp, completedMarker, __ATOMIC_RELEASE);

        state.deltaX = 0;
        state.deltaY = 0;
        state.scrollChangeX = 0;
        state.scrollChangeY = 0;
    }
}
