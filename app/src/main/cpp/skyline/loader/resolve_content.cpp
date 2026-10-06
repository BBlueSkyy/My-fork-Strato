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

        vfs::NCA *base{programNca ? &*programNca : nullptr};
        vfs::NCA *patch{programPatchNca ? &*programPatchNca : nullptr};

        if (state.updateLoader) {
            auto &external{*state.updateLoader};
            auto *externalProgram{
                external.programPatchNca ? &*external.programPatchNca :
                external.programNca ? &*external.programNca : nullptr
            };

            if (!externalProgram) {
                if (base)
                    throw exception("Selected update contains no matching Program NCA");
            } else {
                patch = externalProgram;
                LOGI("ResolveProgramContent: selected external Program update");
            }
        }

        if (!base) {
            if (!patch) {
                currentProcessRomFs = romFs;
                currentProcessRomFsIdentity = DescribeRomFs(currentProcessRomFs);
                programContentResolved = true;
                return;
            }

            // Some multi-program applications add a ProgramIndex only in the update.
            // In that case the update Program NCA is the primary program for this index,
            // not a patch layered over a non-existent base Program NCA.
            LOGI("ResolveProgramContent: promoting update-only Program 0x{:016X} to primary Program (BKTR={}, RomFS={})",
                 patch->header.titleId, patch->HasBktrSection(), patch->HasRomFsSection());

            auto exeFs{patch->OpenExeFs()};
            // A BKTR data section cannot be layered without a base Program NCA. Treat the
            // update NCA as the primary executable and only expose a directly-openable RomFS.
            // This matches update-only indexed-program loading semantics used by Ryujinx.
            auto data{patch->OpenRomFs()};

            if (!exeFs || !exeFs->FileExists("main") || !exeFs->FileExists("main.npdm"))
                throw exception("Resolved update-only Program ExeFS lacks main or main.npdm");

            vfs::PatchManager modifications;
            exeFs = modifications.PatchExeFS(state, exeFs, patch->header.titleId);
            if (data)
                data = modifications.PatchRomFS(state, data, patch->header.titleId);

            if (state.updateLoader) {
                if (state.updateLoader->nacp)
                    nacp = state.updateLoader->nacp;
                if (state.updateLoader->cnmt)
                    cnmt = state.updateLoader->cnmt;
            }

            const auto identity{DescribeRomFs(data)};
            processExeFs = std::move(exeFs);
            currentProcessRomFs = data;
            patchDataRomFs = nullptr;
            romFs = std::move(data);
            currentProcessRomFsIdentity = identity;
            programUpdateApplied = true;
            programContentResolved = true;
            LOGI("ResolveProgramContent: launched update-only Program 0x{:016X} (direct RomFS={})",
                 patch->header.titleId, currentProcessRomFs != nullptr);
            LOGI("Resolved current-process RomFS: {}", identity);
            return;
        }

        LOGI("ResolveProgramContent: base Program 0x{:016X} (BKTR={}, RomFS={})",
             base->header.titleId, base->HasBktrSection(), base->HasRomFsSection());
        if (patch)
            LOGI("ResolveProgramContent: update Program 0x{:016X} (BKTR={}, RomFS={})",
                 patch->header.titleId, patch->HasBktrSection(), patch->HasRomFsSection());

        if (patch && patch->header.titleId != base->header.titleId)
            throw exception("Selected update contains no matching Program NCA");

        std::shared_ptr<vfs::FileSystem> exeFs;
        std::shared_ptr<vfs::Backing> data;
        try {
            LOGI("ResolveProgramContent: opening resolved ExeFS");
            exeFs = patch ? patch->OpenExeFsWithPatch(*base) : base->OpenExeFs();
            LOGI("ResolveProgramContent: ExeFS open complete (present={})", exeFs != nullptr);

            LOGI("ResolveProgramContent: opening resolved RomFS");
            data = patch ? patch->OpenRomFsWithPatch(*base) : base->OpenRomFs();
            LOGI("ResolveProgramContent: RomFS open complete (present={})", data != nullptr);
        } catch (const std::exception &e) {
            LOGE("ResolveProgramContent: Program content open failed: {}", e.what());
            throw;
        }

        if (!exeFs || !exeFs->FileExists("main") || !exeFs->FileExists("main.npdm"))
            throw exception("Resolved Program ExeFS lacks main or main.npdm");

        vfs::PatchManager modifications;
        exeFs = modifications.PatchExeFS(state, exeFs, base->header.titleId);
        if (data)
            data = modifications.PatchRomFS(state, data, base->header.titleId);
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
