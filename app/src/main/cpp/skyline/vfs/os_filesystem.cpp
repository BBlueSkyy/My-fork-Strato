// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits>
#include "os_backing.h"
#include "os_filesystem.h"

namespace skyline::vfs {
    namespace {
        std::error_code ErrnoError() {
            return {errno, std::generic_category()};
        }

        bool IsWithin(const std::filesystem::path &root, const std::filesystem::path &path) {
            auto rootComponent{root.begin()};
            auto pathComponent{path.begin()};
            for (; rootComponent != root.end(); ++rootComponent, ++pathComponent) {
                if (pathComponent == path.end() || *rootComponent != *pathComponent)
                    return false;
            }
            return true;
        }

        std::optional<i32> PathLimit(const std::filesystem::path &path, int name) {
            errno = 0;
            const auto value{pathconf(path.c_str(), name)};
            if (value < 0 || value > std::numeric_limits<i32>::max())
                return std::nullopt;
            return static_cast<i32>(value);
        }
    }

    OsFileSystem::OsFileSystem(const std::string &basePath) : FileSystem() {
        std::error_code error;
        std::filesystem::create_directories(basePath, error);
        if (error)
            throw exception("Error creating the OS filesystem backing directory: {}", error.message());

        rootPath = std::filesystem::canonical(basePath, error);
        if (error || !std::filesystem::is_directory(rootPath))
            throw exception("Invalid OS filesystem backing directory: {}", basePath);

        nameLengthMax = PathLimit(rootPath, _PC_NAME_MAX);
        pathLengthMax = PathLimit(rootPath, _PC_PATH_MAX);
    }

    std::pair<std::filesystem::path, std::error_code> OsFileSystem::ResolvePath(std::string_view guestPath) const {
        if (guestPath.find('\0') != std::string_view::npos || guestPath.find('\\') != std::string_view::npos || guestPath.starts_with("//"))
            return {{}, std::make_error_code(std::errc::invalid_argument)};

        if (guestPath.starts_with('/'))
            guestPath.remove_prefix(1);

        std::filesystem::path relative;
        size_t position{};
        while (position < guestPath.size()) {
            const auto separator{guestPath.find('/', position)};
            const auto component{guestPath.substr(position, separator - position)};
            if (component.empty() || component == "." || component == "..")
                return {{}, std::make_error_code(std::errc::invalid_argument)};
            if (nameLengthMax && component.size() > static_cast<size_t>(*nameLengthMax))
                return {{}, std::make_error_code(std::errc::filename_too_long)};
            relative /= component;
            if (separator == std::string_view::npos)
                break;
            position = separator + 1;
            if (position == guestPath.size())
                break;
        }

        if (pathLengthMax && relative.native().size() > static_cast<size_t>(*pathLengthMax))
            return {{}, std::make_error_code(std::errc::filename_too_long)};

        std::error_code error;
        auto resolved{std::filesystem::weakly_canonical(rootPath / relative, error)};
        if (error)
            return {{}, error};
        if (!IsWithin(rootPath, resolved))
            return {{}, std::make_error_code(std::errc::permission_denied)};
        return {std::move(resolved), {}};
    }

    std::error_code OsFileSystem::CreateFileImpl(const std::string &path, size_t size) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return error;
        if (size > static_cast<size_t>(std::numeric_limits<off_t>::max()))
            return std::make_error_code(std::errc::value_too_large);

        const int fd{open(fullPath.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)};
        if (fd < 0)
            return ErrnoError();

        if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
            error = ErrnoError();
            close(fd);
            std::error_code ignored;
            std::filesystem::remove(fullPath, ignored);
            return error;
        }

        if (close(fd) != 0)
            return ErrnoError();
        return {};
    }

    std::error_code OsFileSystem::DeleteFileImpl(const std::string &path) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return error;

        const auto status{std::filesystem::status(fullPath, error)};
        if (error)
            return error;
        if (!std::filesystem::exists(status))
            return std::make_error_code(std::errc::no_such_file_or_directory);
        if (!std::filesystem::is_regular_file(status))
            return std::make_error_code(std::errc::is_a_directory);

        if (!std::filesystem::remove(fullPath, error) && !error)
            return std::make_error_code(std::errc::no_such_file_or_directory);
        return error;
    }

    std::error_code OsFileSystem::DeleteDirectoryImpl(const std::string &path) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return error;
        if (fullPath == rootPath)
            return std::make_error_code(std::errc::permission_denied);

        const auto status{std::filesystem::status(fullPath, error)};
        if (error)
            return error;
        if (!std::filesystem::exists(status))
            return std::make_error_code(std::errc::no_such_file_or_directory);
        if (!std::filesystem::is_directory(status))
            return std::make_error_code(std::errc::not_a_directory);

        if (!std::filesystem::remove(fullPath, error) && !error)
            return std::make_error_code(std::errc::directory_not_empty);
        return error;
    }

    std::error_code OsFileSystem::DeleteDirectoryRecursivelyImpl(const std::string &path) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return error;
        if (fullPath == rootPath)
            return std::make_error_code(std::errc::permission_denied);

        const auto status{std::filesystem::status(fullPath, error)};
        if (error)
            return error;
        if (!std::filesystem::exists(status))
            return std::make_error_code(std::errc::no_such_file_or_directory);
        if (!std::filesystem::is_directory(status))
            return std::make_error_code(std::errc::not_a_directory);

        const auto removed{std::filesystem::remove_all(fullPath, error)};
        if (error)
            return error;
        return removed == 0 ? std::make_error_code(std::errc::no_such_file_or_directory) : std::error_code{};
    }

    std::error_code OsFileSystem::CleanDirectoryRecursivelyImpl(const std::string &path) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return error;

        const auto status{std::filesystem::status(fullPath, error)};
        if (error)
            return error;
        if (!std::filesystem::exists(status))
            return std::make_error_code(std::errc::no_such_file_or_directory);
        if (!std::filesystem::is_directory(status))
            return std::make_error_code(std::errc::not_a_directory);

        std::filesystem::directory_iterator iterator(fullPath, error);
        if (error)
            return error;
        for (const auto &entry : iterator) {
            std::filesystem::remove_all(entry.path(), error);
            if (error)
                return error;
        }
        return {};
    }

    std::error_code OsFileSystem::RenameFileImpl(const std::string &oldPath, const std::string &newPath) {
        auto [source, sourceError]{ResolvePath(oldPath)};
        if (sourceError)
            return sourceError;
        auto [destination, destinationError]{ResolvePath(newPath)};
        if (destinationError)
            return destinationError;

        std::error_code error;
        const auto sourceStatus{std::filesystem::status(source, error)};
        if (error)
            return error;
        if (!std::filesystem::is_regular_file(sourceStatus))
            return std::filesystem::exists(sourceStatus) ? std::make_error_code(std::errc::is_a_directory) : std::make_error_code(std::errc::no_such_file_or_directory);
        if (std::filesystem::exists(destination, error))
            return error ? error : std::make_error_code(std::errc::file_exists);
        if (error)
            return error;

        std::filesystem::rename(source, destination, error);
        return error;
    }

    std::error_code OsFileSystem::RenameDirectoryImpl(const std::string &oldPath, const std::string &newPath) {
        auto [source, sourceError]{ResolvePath(oldPath)};
        if (sourceError)
            return sourceError;
        auto [destination, destinationError]{ResolvePath(newPath)};
        if (destinationError)
            return destinationError;
        if (source == rootPath)
            return std::make_error_code(std::errc::permission_denied);

        std::error_code error;
        const auto sourceStatus{std::filesystem::status(source, error)};
        if (error)
            return error;
        if (!std::filesystem::is_directory(sourceStatus))
            return std::filesystem::exists(sourceStatus) ? std::make_error_code(std::errc::not_a_directory) : std::make_error_code(std::errc::no_such_file_or_directory);
        if (std::filesystem::exists(destination, error))
            return error ? error : std::make_error_code(std::errc::file_exists);
        if (error)
            return error;

        std::filesystem::rename(source, destination, error);
        return error;
    }

    std::error_code OsFileSystem::CreateDirectoryImpl(const std::string &path, bool parents) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return error;

        const bool created{parents ? std::filesystem::create_directories(fullPath, error) : std::filesystem::create_directory(fullPath, error)};
        if (error)
            return error;
        if (!created)
            return std::make_error_code(std::errc::file_exists);
        return {};
    }

    std::shared_ptr<Backing> OsFileSystem::OpenFileImpl(const std::string &path, Backing::Mode mode) {
        return OpenFileWithErrorImpl(path, mode).first;
    }

    std::pair<std::shared_ptr<Backing>, std::error_code> OsFileSystem::OpenFileWithErrorImpl(const std::string &path, Backing::Mode mode) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return {nullptr, error};

        const int flags{(mode.read && mode.write) ? O_RDWR : (mode.write ? O_WRONLY : O_RDONLY)};
        const int fd{open(fullPath.c_str(), flags | O_CLOEXEC)};
        if (fd < 0)
            return {nullptr, ErrnoError()};
        return {std::make_shared<OsBacking>(fd, true, mode), {}};
    }

    std::optional<Directory::EntryType> OsFileSystem::GetEntryTypeImpl(const std::string &path) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return std::nullopt;

        const auto status{std::filesystem::status(fullPath, error)};
        if (error)
            return std::nullopt;
        if (std::filesystem::is_directory(status))
            return Directory::EntryType::Directory;
        if (std::filesystem::is_regular_file(status))
            return Directory::EntryType::File;
        return std::nullopt;
    }

    std::shared_ptr<Directory> OsFileSystem::OpenDirectoryImpl(const std::string &path, Directory::ListMode listMode) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error || !std::filesystem::is_directory(fullPath, error) || error)
            return nullptr;
        return std::make_shared<OsFileSystemDirectory>(std::move(fullPath), listMode);
    }

    std::error_code OsFileSystem::CommitImpl() {
        const int fd{open(rootPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (fd < 0)
            return ErrnoError();
        if (fsync(fd) != 0) {
            auto error{ErrnoError()};
            close(fd);
            return error;
        }
        if (close(fd) != 0)
            return ErrnoError();
        return {};
    }

    std::error_code OsFileSystem::GetSpaceImpl(const std::string &path, u64 &free, u64 &total) {
        auto [resolved, error]{ResolvePath(path)};
        if (error)
            return error;

        struct statvfs info {};
        if (statvfs(resolved.c_str(), &info) != 0)
            return ErrnoError();
        if (info.f_frsize != 0 && (info.f_blocks > std::numeric_limits<u64>::max() / info.f_frsize || info.f_bavail > std::numeric_limits<u64>::max() / info.f_frsize))
            return std::make_error_code(std::errc::value_too_large);

        total = static_cast<u64>(info.f_blocks) * static_cast<u64>(info.f_frsize);
        free = static_cast<u64>(info.f_bavail) * static_cast<u64>(info.f_frsize);
        return {};
    }

    std::error_code OsFileSystem::GetFileTimeStampImpl(const std::string &path, FileTimeStamp &timestamp) {
        auto [fullPath, error]{ResolvePath(path)};
        if (error)
            return error;

        struct stat info {};
        if (stat(fullPath.c_str(), &info) != 0)
            return ErrnoError();

        timestamp.modified = static_cast<u64>(info.st_mtim.tv_sec);
        timestamp.accessed = static_cast<u64>(info.st_atim.tv_sec);
#if defined(__APPLE__)
        timestamp.created = static_cast<u64>(info.st_birthtimespec.tv_sec);
        timestamp.isValid = true;
#else
        timestamp.created = 0;
        timestamp.isValid = false;
#endif
        return {};
    }

    std::error_code OsFileSystem::GetFileSystemAttributeImpl(FileSystemAttribute &attribute) {
        attribute.directoryNameLengthMax = nameLengthMax;
        attribute.fileNameLengthMax = nameLengthMax;
        attribute.directoryPathLengthMax = pathLengthMax;
        attribute.filePathLengthMax = pathLengthMax;
        return {};
    }

    OsFileSystemDirectory::OsFileSystemDirectory(std::filesystem::path path, Directory::ListMode listMode) : Directory(listMode), path(std::move(path)) {}

    std::vector<Directory::Entry> OsFileSystemDirectory::Read() {
        if (!listMode.file && !listMode.directory)
            return {};

        std::vector<Directory::Entry> outputEntries;
        std::error_code error;
        std::filesystem::directory_iterator iterator(path, error);
        if (error)
            throw exception("Failed to open directory: {}, error: {}", path.string(), error.message());

        for (const auto &entry : iterator) {
            const auto status{entry.symlink_status(error)};
            if (error)
                throw exception("Failed to stat directory entry: {}, error: {}", entry.path().string(), error.message());

            if (std::filesystem::is_directory(status) && listMode.directory) {
                outputEntries.push_back({
                    .name = entry.path().filename().string(),
                    .type = Directory::EntryType::Directory,
                    .size = 0,
                });
            } else if (std::filesystem::is_regular_file(status) && listMode.file) {
                const auto size{entry.file_size(error)};
                if (error)
                    throw exception("Failed to obtain directory entry size: {}, error: {}", entry.path().string(), error.message());
                outputEntries.push_back({
                    .name = entry.path().filename().string(),
                    .type = Directory::EntryType::File,
                    .size = listMode.noFileSize ? 0 : static_cast<size_t>(size),
                });
            }
        }

        return outputEntries;
    }
}
