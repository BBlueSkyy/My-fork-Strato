// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include "filesystem.h"

namespace skyline::vfs {
    class PatchManager {
      public:
        PatchManager();

        // These methods add user mods to content already resolved by the loader.
        std::shared_ptr<vfs::Backing> PatchRomFS(const DeviceState &state, std::shared_ptr<Backing> romFs, u64 titleId);

        std::shared_ptr<FileSystem> PatchExeFS(const DeviceState &state, std::shared_ptr<FileSystem> exefs, u64 titleId);
    };
}
