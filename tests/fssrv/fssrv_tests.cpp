// SPDX-License-Identifier: MPL-2.0

#include <climits>
#include <iostream>
#include <services/fssrv/types.h>
#include <services/fssrv/validation.h>
#include <vfs/os_filesystem.h>

using namespace skyline;
using namespace skyline::service::fssrv;

namespace {
    void Check(bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    void TestAbiLayouts() {
        static_assert(sizeof(SaveDataAttribute) == 0x40);
        static_assert(sizeof(SaveDataInfo) == 0x60);
        static_assert(sizeof(FileTimeStampRaw) == 0x20);
        static_assert(sizeof(FileSystemAttribute) == 0xC0);
    }

    void TestSignedRanges() {
        Check(!ValidateRange(-1, 1, 4), "negative offset accepted");
        Check(!ValidateRange(0, -1, 4), "negative size accepted");
        Check(ValidateRange(4, 0, 4), "zero-sized end range rejected");
        Check(!ValidateRange(4, 1, 4), "past-end range accepted");
        Check(!ValidateRange(INT64_MAX, 1, 16), "unrepresentable range accepted");
    }

    void TestGuestPathParsing() {
        std::array<u8, 8> valid{'/', 'a', '/', 'b', 0, 0xCC, 0xCC, 0xCC};
        auto path{ReadPath(valid)};
        Check(path && *path == "/a/b", "valid guest path was not preserved");

        std::array<u8, 4> traversal{'.', '.', '/', 0};
        Check(!ReadPath(traversal), "parent traversal accepted");

        std::array<u8, 4> backslash{'a', '\\', 'b', 0};
        Check(!ReadPath(backslash), "host-style separator accepted");

        std::array<u8, 4> unterminated{'a', 'b', 'c', 'd'};
        Check(!ReadPath(unterminated), "unterminated path accepted");

        std::array<u8, 0x302> oversized{};
        Check(!ReadPath(oversized), "oversized FspPath accepted");
    }

    class TempDirectory {
      public:
        std::filesystem::path path;

        TempDirectory() {
            std::array<char, 32> pattern{};
            std::strcpy(pattern.data(), "/tmp/strato-fssrv-XXXXXX");
            auto created{mkdtemp(pattern.data())};
            if (!created)
                throw std::runtime_error("failed to create temporary directory");
            path = created;
        }

        ~TempDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    void TestRootedHostMutations() {
        TempDirectory root;
        vfs::OsFileSystem fs(root.path.string());

        Check(!fs.CreateDirectory("inside", false), "rooted directory create failed");
        Check(!fs.CreateFile("inside/file", 4), "rooted file create failed");
        Check(std::filesystem::file_size(root.path / "inside/file") == 4, "created file has wrong size");
        Check(fs.CreateFile("../escape", 1) == std::errc::invalid_argument, "parent escape accepted");
        Check(fs.DeleteFile("missing") == std::errc::no_such_file_or_directory, "missing delete reported success");
        Check(fs.DeleteDirectory("inside") == std::errc::directory_not_empty, "non-empty directory delete reported success");

        Check(!fs.CleanDirectoryRecursively("inside"), "recursive clean failed");
        Check(std::filesystem::is_directory(root.path / "inside"), "recursive clean removed the directory");
        Check(std::filesystem::is_empty(root.path / "inside"), "recursive clean retained children");

        Check(!fs.CreateFile("inside/file", 1), "file recreate failed");
        Check(!fs.DeleteDirectoryRecursively("inside"), "recursive delete failed");
        Check(!std::filesystem::exists(root.path / "inside"), "recursive delete retained the directory");
        Check(std::filesystem::is_directory(root.path), "recursive delete escaped the filesystem root");
    }

    void TestHostRename() {
        TempDirectory root;
        vfs::OsFileSystem fs(root.path.string());
        Check(!fs.CreateDirectory("inside", false), "rename fixture directory failed");
        Check(!fs.CreateFile("inside/file", 1), "rename fixture file failed");
        Check(!fs.RenameFile("inside/file", "inside/renamed"), "file rename failed");
        Check(std::filesystem::is_regular_file(root.path / "inside/renamed"), "file rename did not move the source");
        Check(!fs.RenameDirectory("inside", "moved"), "directory rename failed");
        Check(std::filesystem::is_directory(root.path / "moved"), "directory rename did not move the source");
    }

    void TestHostCommit() {
        TempDirectory root;
        vfs::OsFileSystem fs(root.path.string());
        Check(!fs.Commit(), "host filesystem commit failed");
    }

    void TestSymlinkEscapeIsRejected() {
        TempDirectory root;
        TempDirectory outside;
        std::filesystem::create_directory_symlink(outside.path, root.path / "escape");
        vfs::OsFileSystem fs(root.path.string());

        Check(fs.CreateFile("escape/file", 1) == std::errc::permission_denied, "create followed a symlink outside the root");
        Check(fs.OpenFileUnchecked("escape/file") == nullptr, "open followed a symlink outside the root");
        Check(fs.DeleteFile("escape/file") == std::errc::permission_denied, "delete followed a symlink outside the root");
        Check(!std::filesystem::exists(outside.path / "file"), "an outside file was created");

        const auto entries{fs.OpenDirectory("")->Read()};
        Check(std::ranges::none_of(entries, [](const auto &entry) { return entry.name == "escape"; }), "directory enumeration exposed an outside symlink");
    }

    void TestHostMetadataCapabilities() {
        TempDirectory root;
        vfs::OsFileSystem fs(root.path.string());
        Check(!fs.CreateFile("metadata", 1), "metadata fixture creation failed");

        u64 free{}, total{};
        Check(!fs.GetSpace("/", free, total), "space query failed");
        Check(total > 0 && free <= total, "space query returned impossible values");

        vfs::FileTimeStamp timestamp{};
        Check(!fs.GetFileTimeStamp("metadata", timestamp), "timestamp query failed");
        Check(timestamp.modified > 0 && timestamp.accessed > 0, "timestamps are not POSIX seconds");

        vfs::FileSystemAttribute attribute{};
        Check(!fs.GetFileSystemAttribute(attribute), "filesystem attribute query failed");
        Check(attribute.directoryNameLengthMax && *attribute.directoryNameLengthMax > 0, "directory-name limit unavailable");
        Check(attribute.fileNameLengthMax && *attribute.fileNameLengthMax > 0, "file-name limit unavailable");
    }
}

int main() {
    int failures{};
    const auto run = [&](const char *name, auto test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    };

    run("filesystem ABI layouts", TestAbiLayouts);
    run("signed storage ranges", TestSignedRanges);
    run("guest path parsing", TestGuestPathParsing);
    run("rooted host mutations", TestRootedHostMutations);
    run("host rename", TestHostRename);
    run("host commit", TestHostCommit);
    run("symlink escape rejection", TestSymlinkEscapeIsRejected);
    run("host metadata capabilities", TestHostMetadataCapabilities);
    return failures == 0 ? 0 : 1;
}
