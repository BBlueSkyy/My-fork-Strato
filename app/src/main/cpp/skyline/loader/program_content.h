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

    // Uses CNMT Program records for the requested ProgramIndex, independent of container iteration order.
    // Header-only fallback is limited to ProgramIndex 0 in unambiguous containers without Program records.
    ProgramNcaSelection SelectProgramNcas(std::vector<ProgramNcaCandidate> candidates, const std::vector<vfs::CNMT> &metadata,
                                          u8 programIndex = 0);
}
