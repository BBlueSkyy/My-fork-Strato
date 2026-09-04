// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <common.h>

namespace skyline::cheats {
    struct MemoryRegionExtents {
        u64 base{};
        u64 size{};
    };

    struct CheatProcessMetadata {
        u64 titleId{};
        MemoryRegionExtents mainNso{};
        MemoryRegionExtents heap{};
        MemoryRegionExtents alias{};
        MemoryRegionExtents aslr{};
        std::array<u8, 0x20> buildId{};
    };

    struct CheatDefinition {
        std::array<char, 0x40> name{};
        u32 numOpcodes{};
        std::array<u32, 0x100> opcodes{};
    };

    struct CheatEntry {
        bool enabled{};
        bool master{};
        u32 id{};
        CheatDefinition definition{};
    };
}
