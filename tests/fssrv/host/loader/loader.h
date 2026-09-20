// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <vfs/filesystem.h>
#include <vfs/nca.h>

namespace skyline::loader {
    class Loader {
      public:
        struct Nacp {
            struct {
                u64 saveDataOwnerId{};
            } nacpContents;
        };

        struct Cnmt {
            struct {
                u64 id{};
            } header;
        };

        std::optional<Nacp> nacp;
        std::optional<Cnmt> cnmt;
        std::optional<vfs::NCA> publicNca;
        std::shared_ptr<vfs::Backing> currentProcessRomFs;
        std::shared_ptr<vfs::Backing> patchDataRomFs;
        std::string currentProcessRomFsIdentity;
    };
}
