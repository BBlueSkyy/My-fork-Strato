// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include <os.h>
#include <vfs/nca.h>
#include "patch_manager.h"
#include "region_backing.h"

namespace skyline::vfs {
    PatchManager::PatchManager() {}

    std::shared_ptr<FileSystem> PatchManager::PatchExeFS(const DeviceState &state, std::shared_ptr<FileSystem> exefs) {
        if (!state.updateLoader || !state.updateLoader->programNca || !state.updateLoader->programNca->exeFs)
            throw exception("Update does not contain a usable ExeFS");
        auto updateProgramNCA{state.updateLoader->programNca};
        return updateProgramNCA->exeFs;
    }

    std::shared_ptr<vfs::Backing> PatchManager::PatchRomFS(const DeviceState &state, std::optional<vfs::NCA> nca, u64) {
        if (!state.loader || !state.loader->programNca)
            throw exception("Cannot patch RomFS without a selected base program");

        return PatchRomFS(state, std::move(nca), *state.loader->programNca);
    }

    std::shared_ptr<vfs::Backing> PatchManager::PatchRomFS(const DeviceState &state, std::optional<vfs::NCA> updateNca, const vfs::NCA &baseNca) {
        if (!updateNca || !baseNca.rawRomFs)
            throw exception("Cannot patch RomFS without update and base raw RomFS layers");

        auto newNca{std::make_shared<vfs::NCA>(std::move(updateNca), state.os->keyStore, baseNca.rawRomFs, baseNca.ivfcOffset)};
        return newNca->romFs;
    }
}
