// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <unordered_set>
#include "filesystem.h"

namespace skyline::vfs {
    class LayeredFileSystemDirectory final : public Directory {
      private:
        std::vector<Entry> entries;

      public:
        LayeredFileSystemDirectory(std::vector<Entry> entries, ListMode listMode)
            : Directory(listMode), entries(std::move(entries)) {}

        std::vector<Entry> Read() override {
            return entries;
        }
    };

    /**
     * @brief Read-only filesystem overlay. Earlier layers have priority over later layers.
     */
    class LayeredFileSystem final : public FileSystem {
      private:
        std::vector<std::shared_ptr<FileSystem>> layers;

      protected:
        std::shared_ptr<Backing> OpenFileImpl(const std::string &path, Backing::Mode mode) override {
            if (mode.write)
                throw exception("Layered filesystem is read-only");

            for (const auto &layer : layers) {
                if (layer && layer->FileExists(path))
                    return layer->OpenFileUnchecked(path, mode);
            }

            return nullptr;
        }

        std::optional<Directory::EntryType> GetEntryTypeImpl(const std::string &path) override {
            for (const auto &layer : layers) {
                if (!layer)
                    continue;

                auto type{layer->GetEntryType(path)};
                if (type)
                    return type;
            }

            return std::nullopt;
        }

        std::shared_ptr<Directory> OpenDirectoryImpl(const std::string &path, Directory::ListMode listMode) override {
            std::vector<Directory::Entry> entries;
            std::unordered_set<std::string> seen;
            bool found{};

            for (const auto &layer : layers) {
                if (!layer || !layer->DirectoryExists(path))
                    continue;

                auto directory{layer->OpenDirectoryUnchecked(path, listMode)};
                if (!directory)
                    continue;

                found = true;
                for (auto &entry : directory->Read()) {
                    if (seen.emplace(entry.name).second)
                        entries.emplace_back(std::move(entry));
                }
            }

            if (!found)
                return nullptr;

            return std::make_shared<LayeredFileSystemDirectory>(std::move(entries), listMode);
        }

      public:
        explicit LayeredFileSystem(std::vector<std::shared_ptr<FileSystem>> layers)
            : layers(std::move(layers)) {
            if (this->layers.empty())
                throw exception("Layered filesystem requires at least one layer");
        }
    };
}
