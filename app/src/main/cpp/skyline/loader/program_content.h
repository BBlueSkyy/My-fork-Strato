// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <vfs/nca.h>
#include <vfs/cnmt.h>

namespace skyline::loader {
    struct ProgramNcaCandidate {
        std::string filename;
        vfs::NCA nca;
    };

    struct ProgramNcaSelection {
        std::optional<vfs::NCA> base;
        std::optional<vfs::NCA> patch;
        std::optional<vfs::CNMT> metadata;
    };

    // Uses CNMT Program records (idOffset 0), independent of container iteration order.
    // Header-only fallback is limited to unambiguous containers without Program records.
    ProgramNcaSelection SelectProgramNcas(std::vector<ProgramNcaCandidate> candidates, const std::vector<vfs::CNMT> &metadata);
}
