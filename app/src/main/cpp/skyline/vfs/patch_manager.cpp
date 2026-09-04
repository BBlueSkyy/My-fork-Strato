// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <os.h>
#include <vfs/nca.h>
#include <vfs/os_filesystem.h>
#include "layered_filesystem.h"
#include "patch_manager.h"
#include "region_backing.h"

namespace skyline::vfs {
    namespace {
        std::string ToLower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::optional<std::filesystem::path> FindSubdirectoryCaseless(const std::filesystem::path &parent, std::string_view name) {
            std::error_code error;
            for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error)) {
                if (it->is_directory(error) && !error && ToLower(it->path().filename().string()) == name)
                    return it->path();
            }
            return std::nullopt;
        }

        std::vector<std::filesystem::path> GetModificationDirectories(const DeviceState &state, u64 titleId) {
            const auto root{std::filesystem::path(state.os->publicAppFilesPath) / "switch" / "load" / fmt::format("{:016X}", titleId)};
            std::vector<std::filesystem::path> directories;
            std::error_code error;

            if (!std::filesystem::is_directory(root, error) || error)
                return directories;

            for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
                if (it->is_directory(error) && !error)
                    directories.push_back(it->path());
            }

            std::sort(directories.begin(), directories.end(), [](const auto &left, const auto &right) {
                return left.filename().string() < right.filename().string();
            });
            return directories;
        }
    }

    PatchManager::PatchManager() {}

    std::shared_ptr<FileSystem> PatchManager::PatchExeFS(const DeviceState &state, std::shared_ptr<FileSystem> exefs, u64 titleId) {
        if (!exefs)
            throw exception("Cannot patch a null ExeFS");

        std::shared_ptr<FileSystem> patchedExeFs{std::move(exefs)};
        if (state.updateLoader && state.updateLoader->programNca && state.updateLoader->programNca->exeFs) {
            patchedExeFs = state.updateLoader->programNca->exeFs;
            LOGI("ExeFS: applied update layer");
        }

        std::vector<std::shared_ptr<FileSystem>> layers;
        for (const auto &modDirectory : GetModificationDirectories(state, titleId)) {
            auto exefsDirectory{FindSubdirectoryCaseless(modDirectory, "exefs")};
            if (!exefsDirectory)
                continue;

            std::error_code error;
            if (std::filesystem::directory_iterator(*exefsDirectory, error) == std::filesystem::directory_iterator{} || error)
                continue;

            layers.emplace_back(std::make_shared<OsFileSystem>(exefsDirectory->string()));
            LOGI("ExeFS: applying LayeredExeFS mod '{}'", modDirectory.filename().string());
        }

        if (layers.empty())
            return patchedExeFs;

        layers.emplace_back(std::move(patchedExeFs));
        return std::make_shared<LayeredFileSystem>(std::move(layers));
    }

    std::shared_ptr<vfs::Backing> PatchManager::PatchRomFS(const DeviceState &state, std::optional<vfs::NCA> nca, u64 ivfcOffset) {
        if (!nca || !state.loader || !state.loader->programNca || !state.loader->programNca->rawRomFs)
            throw exception("Cannot patch RomFS without update and base raw RomFS layers");

        auto newNca{std::make_shared<vfs::NCA>(std::move(nca), state.os->keyStore, state.loader->programNca->rawRomFs, ivfcOffset)};
        return newNca->romFs;
    }
}
