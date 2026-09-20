// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <filesystem>
#include "filesystem.h"

namespace skyline::vfs {
    /**
     * @brief The OsFileSystem class abstracts an OS folder with the vfs::FileSystem api
     */
    class OsFileSystem : public FileSystem {
      private:
        struct ExistingRoot {};

        std::filesystem::path rootPath;
        std::optional<i32> nameLengthMax;
        std::optional<i32> pathLengthMax;

        OsFileSystem(std::filesystem::path rootPath, ExistingRoot);

        std::pair<std::filesystem::path, std::error_code> ResolvePath(std::string_view guestPath) const;

      protected:
        std::error_code CreateFileImpl(const std::string &path, size_t size) override;

        std::error_code DeleteFileImpl(const std::string &path) override;

        std::error_code DeleteDirectoryImpl(const std::string &path) override;

        std::error_code DeleteDirectoryRecursivelyImpl(const std::string &path) override;

        std::error_code CleanDirectoryRecursivelyImpl(const std::string &path) override;

        std::error_code RenameFileImpl(const std::string &oldPath, const std::string &newPath) override;

        std::error_code RenameDirectoryImpl(const std::string &oldPath, const std::string &newPath) override;

        std::error_code CreateDirectoryImpl(const std::string &path, bool parents) override;

        std::error_code CommitImpl() override;

        std::error_code GetSpaceImpl(const std::string &path, u64 &free, u64 &total) override;

        std::error_code GetFileTimeStampImpl(const std::string &path, FileTimeStamp &timestamp) override;

        std::error_code GetFileSystemAttributeImpl(FileSystemAttribute &attribute) override;

        std::pair<std::shared_ptr<Backing>, std::error_code> OpenFileWithErrorImpl(const std::string &path, Backing::Mode mode) override;

        std::shared_ptr<Backing> OpenFileImpl(const std::string &path, Backing::Mode mode) override;

        std::optional<Directory::EntryType> GetEntryTypeImpl(const std::string &path) override;

        std::shared_ptr<Directory> OpenDirectoryImpl(const std::string &path, Directory::ListMode listMode) override;

        bool IsReadOnlyImpl() const override { return false; }

      public:
        OsFileSystem(const std::string &basePath);

        static std::pair<std::shared_ptr<OsFileSystem>, std::error_code> OpenExisting(const std::string &basePath);
    };

    /**
     * @brief OsFileSystemDirectory abstracts access to a native linux directory through the VFS APIs
     */
    class OsFileSystemDirectory : public Directory {
      private:
        std::filesystem::path path;

      public:
        OsFileSystemDirectory(std::filesystem::path path, ListMode listMode);

        std::vector<Entry> Read();
    };
}
