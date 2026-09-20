// SPDX-License-Identifier: MPL-2.0

#include <climits>
#include <iostream>
#include <services/fssrv/types.h>
#include <services/fssrv/validation.h>
#include <services/fssrv/helpers.h>
#include <services/fssrv/IFile.h>
#include <services/fssrv/IStorage.h>
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

    void TestFileSystemServiceHelpers() {
        Check(MapVfsError(std::make_error_code(std::errc::no_such_file_or_directory)) == result::PathDoesNotExist, "missing path mapped incorrectly");
        Check(MapVfsError(std::make_error_code(std::errc::file_exists)) == result::PathAlreadyExists, "existing path mapped incorrectly");
        Check(MapVfsError(std::make_error_code(std::errc::filename_too_long)) == result::TooLongPath, "long path mapped incorrectly");

        vfs::Backing::Mode mode{};
        Check(!IsOpenModeValid(mode), "empty open mode accepted");
        mode.raw = 8;
        Check(!IsOpenModeValid(mode), "unknown open-mode bit accepted");
        mode = {true, false, false};
        Check(IsOpenModeValid(mode), "read-only open mode rejected");
        Check(IsMutationAllowed(false, mode), "read open rejected on writable filesystem");
        mode = {true, true, false};
        Check(!IsMutationAllowed(true, mode), "write open accepted on read-only filesystem");
        Check(IsDirectoryModeValid(0x1), "directory-only mode rejected");
        Check(IsDirectoryModeValid(0x80000002), "no-size file mode rejected");
        Check(!IsDirectoryModeValid(0), "empty directory mode accepted");
        Check(!IsDirectoryModeValid(0x4), "unknown directory-mode bit accepted");

        Check(!ToSize(-1), "negative size converted to size_t");
        Check(ToSize(4) && *ToSize(4) == 4, "valid size conversion failed");
    }

    void TestPaginationBounds() {
        Check(CalculateReadCount(3, 0, 2) == 2, "first page count is wrong");
        Check(CalculateReadCount(3, 2, 2) == 1, "second page count is wrong");
        Check(CalculateReadCount(3, 3, 2) == 0, "exhausted page is non-zero");
        Check(CalculateReadCount(3, 4, 2) == 0, "past-end cursor underflowed");
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
        Check(!fs.IsReadOnly(), "host filesystem reports read-only");

        auto [missingFile, missingError]{fs.OpenFileWithError("missing")};
        Check(!missingFile && missingError == std::errc::no_such_file_or_directory, "open-file error was discarded");

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

        vfs::Directory::ListMode noSizeMode{};
        noSizeMode.raw = 0x80000002;
        const auto entries{fs.OpenDirectory("", noSizeMode)->Read()};
        Check(entries.size() == 1 && entries[0].size == 0, "no-file-size directory mode exposed a size");
    }

    void TestBackingCapabilities() {
        TempDirectory root;
        vfs::OsFileSystem fs(root.path.string());
        Check(!fs.CreateFile("data", 4), "backing fixture creation failed");

        vfs::Backing::Mode writableMode{true, true, false};
        auto writable{fs.OpenFile("data", writableMode)};
        std::array<u8, 2> input{0x12, 0x34};
        auto [written, writeError]{writable->WriteWithError(input, 1)};
        Check(!writeError && written == input.size(), "bounded backing write failed");
        Check(!writable->Flush(), "backing flush failed");
        Check(writable->WriteWithError(input, 3).second == std::errc::result_out_of_range, "past-end backing write succeeded");
        Check(!writable->ResizeWithError(6) && writable->size == 6, "backing resize failed");

        auto readOnly{fs.OpenFile("data")};
        Check(readOnly->WriteWithError(input, 0).second == std::errc::read_only_file_system, "read-only backing accepted a write");
        Check(readOnly->ResizeWithError(2) == std::errc::read_only_file_system, "read-only backing accepted a resize");

        std::array<u8, 2> output{};
        auto [read, readError]{readOnly->ReadWithError(output, 1)};
        Check(!readError && read == output.size() && output == input, "checked backing read failed");
        Check(readOnly->ReadWithError(output, 5).second == std::errc::result_out_of_range, "past-end backing read succeeded");
    }

    struct RangeInput {
        i64 offset;
        i64 size;
    };

    struct FileIoInput {
        u32 option;
        u32 padding;
        i64 offset;
        i64 size;
    };

    template<typename T>
    ipc::IpcRequest RequestWith(const T &input) {
        ipc::IpcRequest request;
        request.cmdStorage.resize(sizeof(T));
        std::memcpy(request.cmdStorage.data(), &input, sizeof(T));
        request.cmdArg = request.cmdStorage.data();
        request.cmdArgSz = sizeof(T);
        return request;
    }

    void TestStorageServiceBoundsAndPermissions() {
        TempDirectory root;
        vfs::OsFileSystem fs(root.path.string());
        Check(!fs.CreateFile("storage", 4), "storage fixture creation failed");
        auto writable{fs.OpenFile("storage", {true, true, false})};
        DeviceState state;
        service::ServiceManager manager;
        kernel::type::KSession session;
        IStorage storage(writable, state, manager);

        std::array<u8, 4> output{};
        auto negativeOffset{RequestWith(RangeInput{-1, 1})};
        negativeOffset.outputBuf.emplace_back(output);
        ipc::IpcResponse response;
        Check(storage.Read(session, negativeOffset, response) == result::InvalidOffset, "negative storage offset accepted");

        auto overflow{RequestWith(RangeInput{3, 2})};
        overflow.outputBuf.emplace_back(output);
        Check(storage.Read(session, overflow, response) == result::OutOfRange, "past-end storage read accepted");

        auto shortBuffer{RequestWith(RangeInput{0, 4})};
        shortBuffer.outputBuf.emplace_back(output.data(), 2);
        Check(storage.Read(session, shortBuffer, response) == result::InvalidSize, "short storage output buffer accepted");

        std::array<u8, 2> input{0xA5, 0x5A};
        auto write{RequestWith(RangeInput{1, 2})};
        write.inputBuf.emplace_back(input);
        Check(!storage.Write(session, write, response), "bounded storage write failed");
        Check(!storage.Flush(session, write, response), "storage flush failed");

        auto readOnly{fs.OpenFile("storage")};
        IStorage readOnlyStorage(readOnly, state, manager);
        Check(readOnlyStorage.Write(session, write, response) == result::WriteNotPermitted, "read-only storage accepted a write");
        Check(readOnlyStorage.SetSize(session, write, response) == result::WriteNotPermitted, "read-only storage accepted resize");
    }

    void TestFileServiceIo() {
        TempDirectory root;
        vfs::OsFileSystem fs(root.path.string());
        Check(!fs.CreateFile("file", 4), "file fixture creation failed");
        auto backing{fs.OpenFile("file", {true, true, true})};
        DeviceState state;
        service::ServiceManager manager;
        kernel::type::KSession session;
        IFile file(backing, state, manager);

        std::array<u8, 2> input{0x11, 0x22};
        auto write{RequestWith(FileIoInput{0, 0, 3, 2})};
        write.inputBuf.emplace_back(input);
        ipc::IpcResponse response;
        Check(!file.Write(session, write, response) && backing->size == 5, "append-capable file was not extended");

        std::array<u8, 4> output{};
        auto read{RequestWith(FileIoInput{0, 0, 3, 4})};
        read.outputBuf.emplace_back(output);
        Check(!file.Read(session, read, response), "bounded file read failed");
        Check(response.Get<u64>() == 2 && output[0] == 0x11 && output[1] == 0x22, "file read did not report the EOF-limited size");

        auto badOption{RequestWith(FileIoInput{2, 0, 0, 0})};
        Check(file.Write(session, badOption, response) == result::InvalidArgument, "unknown file write option accepted");
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
    run("filesystem service helpers", TestFileSystemServiceHelpers);
    run("pagination bounds", TestPaginationBounds);
    run("rooted host mutations", TestRootedHostMutations);
    run("host rename", TestHostRename);
    run("host commit", TestHostCommit);
    run("symlink escape rejection", TestSymlinkEscapeIsRejected);
    run("host metadata capabilities", TestHostMetadataCapabilities);
    run("backing capabilities", TestBackingCapabilities);
    run("storage service bounds and permissions", TestStorageServiceBoundsAndPermissions);
    run("file service IO", TestFileServiceIo);
    return failures == 0 ? 0 : 1;
}
