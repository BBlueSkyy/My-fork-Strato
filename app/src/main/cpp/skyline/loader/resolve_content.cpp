// SPDX-License-Identifier: MPL-2.0
#include "loader.h"
#include <vfs/patch_manager.h>

namespace skyline::loader {
    namespace {
        std::string DescribeRomFs(const std::shared_ptr<vfs::Backing> &backing) {
            if (!backing)
                return "unavailable";
            std::array<u8, 16> first{};
            const size_t firstSize{std::min(first.size(), backing->size)};
            if (backing->Read(span(first).first(firstSize)) != firstSize)
                throw exception("Short resolved RomFS header read");
            std::string description{fmt::format("size=0x{:X}, first16=", backing->size)};
            for (size_t i{}; i < firstSize; ++i)
                description += fmt::format("{:02X}", first[i]);
            // FNV-1a/64, up to 4096 bytes at each offset. Diagnostic fingerprint, not integrity verification.
            const size_t blockSize{std::min<size_t>(4096, backing->size)};
            if (blockSize != 0) {
                const std::array<size_t, 3> offsets{0, ((backing->size - blockSize) / 2) & ~size_t{15}, backing->size - blockSize};
                std::vector<u8> block(blockSize);
                for (const auto offset : offsets) {
                    if (backing->Read(block, offset) != block.size())
                        throw exception("Short resolved RomFS fingerprint read");
                    u64 hash{14695981039346656037ULL};
                    for (const u8 byte : block)
                        hash = (hash ^ byte) * 1099511628211ULL;
                    description += fmt::format(", fnv1a64[0x{:X}+0x{:X}]=0x{:016X}", offset, blockSize, hash);
                }
            }
            return description;
        }
    }

    void Loader::ResolveProgramContent(const DeviceState &state) {
        if (programContentResolved)
            return;
        if (!programNca) {
            if (programPatchNca)
                throw exception("A Program patch requires its base application");
            currentProcessRomFs = romFs;
            currentProcessRomFsIdentity = DescribeRomFs(currentProcessRomFs);
            programContentResolved = true;
            return;
        }

        vfs::NCA *patch{programPatchNca ? &*programPatchNca : nullptr};
        if (state.updateLoader) {
            auto &external{*state.updateLoader};
            patch = external.programPatchNca ? &*external.programPatchNca : external.programNca ? &*external.programNca : nullptr;
            if (!patch || patch->header.titleId != programNca->header.titleId)
                throw exception("Selected update contains no matching Program NCA");
            LOGI("ResolveProgramContent: selected external Program update");
        }

        auto exeFs{patch ? patch->OpenExeFsWithPatch(*programNca) : programNca->exeFs};
        auto data{patch ? patch->OpenRomFsWithPatch(*programNca) : programNca->romFs};
        if (!exeFs || !exeFs->FileExists("main") || !exeFs->FileExists("main.npdm"))
            throw exception("Resolved Program ExeFS lacks main or main.npdm");

        vfs::PatchManager modifications;
        exeFs = modifications.PatchExeFS(state, exeFs, programNca->header.titleId);
        if (data)
            data = modifications.PatchRomFS(state, data, programNca->header.titleId);
        const auto identity{DescribeRomFs(data)};
        processExeFs = std::move(exeFs);
        currentProcessRomFs = data;
        // Cmd 203 opens patch data. An ExeFS-only update does not invent a patch RomFS.
        patchDataRomFs = patch && patch->HasRomFsSection() ? data : nullptr;
        romFs = std::move(data); // Keep other loader consumers on the same final view.
        currentProcessRomFsIdentity = identity;
        programUpdateApplied = patch != nullptr;
        programContentResolved = true;
        LOGI("Resolved current-process RomFS: {}", identity);
    }
}
