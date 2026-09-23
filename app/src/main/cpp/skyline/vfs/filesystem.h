// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <optional>
#include <system_error>
#include "backing.h"
#include "directory.h"

namespace skyline::vfs {
    struct FileTimeStamp {
        u64 created{};
        u64 modified{};
        u64 accessed{};
        bool isValid{};
    };

    struct FileSystemAttribute {
        std::optional<i32> directoryNameLengthMax;
        std::optional<i32> fileNameLengthMax;
        std::optional<i32> directoryPathLengthMax;
        std::optional<i32> filePathLengthMax;
    };

    /**
     * @brief The FileSystem class represents an abstract filesystem with child files and folders
     */
    class FileSystem {
      protected:
        virtual std::error_code CreateFileImpl(const std::string &path, size_t size) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code DeleteFileImpl(const std::string &path) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code DeleteDirectoryImpl(const std::string &path) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code DeleteDirectoryRecursivelyImpl(const std::string &path) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code CleanDirectoryRecursivelyImpl(const std::string &path) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code RenameFileImpl(const std::string &oldPath, const std::string &newPath) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code RenameDirectoryImpl(const std::string &oldPath, const std::string &newPath) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code CreateDirectoryImpl(const std::string &path, bool parents) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code CommitImpl() { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code GetSpaceImpl(const std::string &path, u64 &free, u64 &total) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code GetFileTimeStampImpl(const std::string &path, FileTimeStamp &timestamp) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::error_code GetFileSystemAttributeImpl(FileSystemAttribute &attribute) { return std::make_error_code(std::errc::operation_not_supported); }

        virtual std::pair<std::shared_ptr<Backing>, std::error_code> OpenFileWithErrorImpl(const std::string &path, Backing::Mode mode) {
            auto file{OpenFileImpl(path, mode)};
            return {file, file ? std::error_code{} : std::make_error_code(std::errc::no_such_file_or_directory)};
        }

        virtual std::shared_ptr<Backing> OpenFileImpl(const std::string &path, Backing::Mode mode) = 0;

        virtual std::optional<Directory::EntryType> GetEntryTypeImpl(const std::string &path) = 0;

        virtual std::shared_ptr<Directory> OpenDirectoryImpl(const std::string &path, Directory::ListMode listMode) {
            throw exception("This filesystem does not support opening directories");
        };

        virtual bool IsReadOnlyImpl() const { return true; }

      public:
        FileSystem() = default;

        /* Delete the move constructor to prevent multiple instances of the same filesystem */
        FileSystem(const FileSystem &) = delete;

        FileSystem &operator=(const FileSystem &) = delete;

        virtual ~FileSystem() = default;

        /**
         * @brief Creates a file in the filesystem with the requested size
         * @param path The path where the file should be created
         * @param size The size of the file to create
         * @return Whether creating the file succeeded
         */
        std::error_code CreateFile(const std::string &path, size_t size) {
            return CreateFileImpl(path, size);
        }

        std::error_code DeleteFile(const std::string &path) {
            return DeleteFileImpl(path);
        }

        std::error_code DeleteDirectory(const std::string &path) {
            return DeleteDirectoryImpl(path);
        }

        std::error_code DeleteDirectoryRecursively(const std::string &path) {
            return DeleteDirectoryRecursivelyImpl(path);
        }

        std::error_code CleanDirectoryRecursively(const std::string &path) {
            return CleanDirectoryRecursivelyImpl(path);
        }

        std::error_code RenameFile(const std::string &oldPath, const std::string &newPath) {
            return RenameFileImpl(oldPath, newPath);
        }

        std::error_code RenameDirectory(const std::string &oldPath, const std::string &newPath) {
            return RenameDirectoryImpl(oldPath, newPath);
        }
       
         /**
         * @brief Creates a directory in the filesystem
         * @param path The path to where the directory should be created
         * @param parents Whether all parent directories in the given path should be created
         * @return Whether creating the directory succeeded
         */
        std::error_code CreateDirectory(const std::string &path, bool parents) {
            return CreateDirectoryImpl(path, parents);
        }

        std::error_code Commit() { return CommitImpl(); }

        std::error_code GetSpace(const std::string &path, u64 &free, u64 &total) { return GetSpaceImpl(path, free, total); }

        std::error_code GetFileTimeStamp(const std::string &path, FileTimeStamp &timestamp) { return GetFileTimeStampImpl(path, timestamp); }

        std::error_code GetFileSystemAttribute(FileSystemAttribute &attribute) { return GetFileSystemAttributeImpl(attribute); }

        std::pair<std::shared_ptr<Backing>, std::error_code> OpenFileWithError(const std::string &path, Backing::Mode mode = {true, false, false}) {
            if (!mode.read && !mode.write)
                return {nullptr, std::make_error_code(std::errc::invalid_argument)};
            return OpenFileWithErrorImpl(path, mode);
        }

        bool IsReadOnly() const { return IsReadOnlyImpl(); }

        /**
         * @brief Opens a file from the specified path in the filesystem
         * @param path The path to the file
         * @param mode The mode to open the file with
         * @return A shared pointer to a Backing object of the file (may be nullptr)
         */
        std::shared_ptr<Backing> OpenFileUnchecked(const std::string &path, Backing::Mode mode = {true, false, false}) {
            if (!mode.write && !mode.read)
                throw exception("Cannot open a file with a mode that is neither readable nor writable");

            return OpenFileImpl(path, mode);
        }

        /**
         * @brief Opens a file from the specified path in the filesystem and throws an exception if opening fails
         * @param path The path to the file
         * @param mode The mode to open the file with
         * @return A shared pointer to a Backing object of the file
         */
        std::shared_ptr<Backing> OpenFile(const std::string &path, Backing::Mode mode = {true, false, false}) {
            auto file{OpenFileUnchecked(path, mode)};
            if (file == nullptr)
                throw exception("Failed to open file: {}", path);

            return file;
        }

        /**
         * @brief Queries the type of the entry given by path
         * @param path The path to the entry
         * @return The type of the entry, if present
         */
        std::optional<Directory::EntryType> GetEntryType(const std::string &path) {
            return GetEntryTypeImpl(path);
        }

        /**
         * @brief Checks if a given file exists in a filesystem
         * @param path The path to the file
         * @return Whether the file exists
         */
        bool FileExists(const std::string &path) {
            auto entry{GetEntryType(path)};
            return entry && *entry == Directory::EntryType::File;
        }

        /**
         * @brief Checks if a given directory exists in a filesystem
         * @param path The path to the directory
         * @return Whether the directory exists
         */
        bool DirectoryExists(const std::string &path) {
            auto entry{GetEntryType(path)};
            return entry && *entry == Directory::EntryType::Directory;
        }

        /**
         * @brief Opens a directory from the specified path in the filesystem
         * @param path The path to the directory
         * @param listMode The list mode for the directory
         * @return A shared pointer to a Directory object of the directory (may be nullptr)
         */
        std::shared_ptr<Directory> OpenDirectoryUnchecked(const std::string &path, Directory::ListMode listMode = {true, true}) {
            if (!listMode.raw)
                throw exception("Cannot open a directory with an empty listMode");

            return OpenDirectoryImpl(path, listMode);
        };

        /**
         * @brief Opens a directory from the specified path in the filesystem and throws an exception if opening fails
         * @param path The path to the directory
         * @param listMode The list mode for the directory
         * @return A shared pointer to a Directory object of the directory
         */
        std::shared_ptr<Directory> OpenDirectory(const std::string &path, Directory::ListMode listMode = {true, true}) {
            return OpenDirectoryUnchecked(path, listMode);
        };
    };
}
