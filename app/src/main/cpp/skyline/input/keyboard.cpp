// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "keyboard.h"

namespace skyline::input {
    KeyboardManager::KeyboardManager(HidSharedMemory *hid) : section{hid->keyboard} {}

    void KeyboardManager::Activate() {
        std::scoped_lock lock{mutex};
        activated = true;
    }

    void KeyboardManager::SetKeyState(u32 usage, bool pressed, u32 modifiers) {
        std::scoped_lock lock{mutex};
        if (usage >= state.keysDown.size() * 8)
            return;

        const auto byte{usage / 8};
        const auto bit{static_cast<u8>(1U << (usage % 8))};
        if (pressed)
            state.keysDown[byte] |= bit;
        else
            state.keysDown[byte] &= static_cast<u8>(~bit);

        state.modifiers.raw = modifiers & 0x1F1F;
        state.attributes.connected = true;
    }

    void KeyboardManager::UpdateSharedMemory() {
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
        entry.modifiers = state.modifiers;
        entry.attributes = state.attributes;
        entry.keysDown = state.keysDown;
        __atomic_store_n(&entry.globalTimestamp, completedMarker, __ATOMIC_RELEASE);
    }
}
