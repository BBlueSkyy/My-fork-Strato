// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <os.h>
#include <vfs/os_filesystem.h>
#include <vfs/rom_filesystem.h>
#include <vfs/nca.h>
#include <services/ngc/content_reader.h>

using namespace skyline;
using service::ngc::ContentReader;

template<class T> void Put(std::vector<u8> &bytes, size_t offset, const T &value) {
    assert(offset <= bytes.size() && sizeof(value) <= bytes.size() - offset);
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<u8> Archive(u64 titleId, vfs::NCAContentType type, u8 version) {
    using R = vfs::RomFileSystem;
    // A real minimal RomFS is wrapped in the host NCA envelope. All VFS lookup
    // and bounded reads below execute production code.
    std::vector<u8> rom(0x104);
    Put(rom, 0, R::RomFsHeader{0x50, 0x50, 0, 0x50, 0x18, 0x68, 0, 0x68, 0x2c, 0x100});
    Put(rom, 0x50, R::RomFsDirectoryEntry{0, UINT32_MAX, UINT32_MAX, 0, 0, 0});
    Put(rom, 0x68, R::RomFsFileEntry{0, UINT32_MAX, 0, 4, 0, 11});
    std::memcpy(rom.data() + 0x68 + sizeof(R::RomFsFileEntry), "version.dat", 11);
    rom.back() = version;
    std::vector<u8> bytes(16 + rom.size());
    Put(bytes, 0, titleId);
    Put(bytes, 8, static_cast<u32>(type));
    std::copy(rom.begin(), rom.end(), bytes.begin() + 16);
    return bytes;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::filesystem::path root{argv[1]};
    std::filesystem::create_directories(root);
    const auto write = [&](const char *name, std::initializer_list<unsigned char> bytes) {
        std::ofstream file(root / name, std::ios::binary);
        for (auto b : bytes) file.put(static_cast<char>(b));
    };
    unsigned mounts{};
    ContentReader reader{[&]() -> std::shared_ptr<vfs::FileSystem> {
        ++mounts;
        return vfs::OsFileSystem::OpenExisting(root.string()).first;
    }};
    std::vector<u8> data{99};
    assert(!reader.ReadFile("version.dat", data) && data.empty());
    assert(mounts == 1);
    write("version.dat", {0x01,0x02,0x03,0x04});
    assert(reader.Version() == 0x01020304);
    assert(reader.ReadFile("version.dat", data) && data == std::vector<u8>({1,2,3,4}));
    assert(!reader.ReadFile("../outside", data) && data.empty());
    assert(!reader.ReadFile("/version.dat", data));
    assert(!reader.ReadFile("nested/version.dat", data));
    assert(!reader.ReadFile(std::string_view("version.dat\0ignored", 19), data));
    write("version.dat", {0x01,0x02,0x03});
    assert(reader.Version() == 0);
    assert(reader.Reload());
    assert(mounts == 2);

    // No provider must never produce fake bytes; a reload permits a later import.
    bool available{};
    ContentReader missing{[&]() -> std::shared_ptr<vfs::FileSystem> {
        ++mounts;
        return available ? vfs::OsFileSystem::OpenExisting(root.string()).first : nullptr;
    }};
    const auto before{mounts};
    assert(!missing.ReadFile("version.dat", data));
    assert(!missing.ReadFile("version.dat", data));
    assert(mounts == before + 1);
    available = true;
    assert(missing.Reload());
    assert(missing.ReadFile("version.dat", data));

    // Default provider has no filesystem side effects and uses the documented
    // extracted system-title root when no registered NCA has been supplied.
    kernel::OS os;
    os.publicAppFilesPath = (root / "public").string();
    os.privateAppFilesPath = (root / "private").string();
    DeviceState state{&os};
    ContentReader firmware{state};
    assert(!firmware.ReadFile("version.dat", data));
    assert(!std::filesystem::exists(root / "public"));
    auto dictionaryRoot{root / "public/switch/ngc/0100000000000823"};
    std::filesystem::create_directories(dictionaryRoot);
    std::ofstream(dictionaryRoot / "version.dat", std::ios::binary).write("\0\0\0\x2A",4);
    assert(firmware.Reload());
    assert(firmware.Version() == 42);
    std::ofstream(dictionaryRoot / "huge").close();
    std::filesystem::resize_file(dictionaryRoot / "huge", ContentReader::MaxFileSize + 1);
    assert(!firmware.ReadFile("huge", data) && data.empty());

    auto registered{root / "public/switch/nand/system/Contents/registered"};
    std::filesystem::create_directories(registered);
    const auto writeArchive = [&](const char *name, u64 title, vfs::NCAContentType type, u8 version) {
        const auto bytes{Archive(title, type, version)};
        std::ofstream(registered / name, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    };
    writeArchive("wrong-title.nca", 0x0100000000000824, vfs::NCAContentType::Data, 99);
    writeArchive("wrong-type.nca", 0x0100000000000823, vfs::NCAContentType::Program, 99);
    writeArchive("dictionary.nca", 0x0100000000000823, vfs::NCAContentType::Data, 9);
    assert(firmware.Reload() && firmware.Version() == 42); // extracted root precedence
    std::filesystem::remove_all(dictionaryRoot);
    assert(firmware.Reload() && firmware.Version() == 9);
    std::filesystem::create_directories(registered / "duplicate.nca");
    writeArchive("duplicate.nca/00", 0x0100000000000823, vfs::NCAContentType::Data, 10);
    assert(!firmware.Reload() && firmware.Version() == 0); // ambiguous firmware versions
    std::filesystem::remove(registered / "dictionary.nca");
    assert(firmware.Reload() && firmware.Version() == 10); // directory content archive

    auto cyclic{Archive(0x0100000000000823, vfs::NCAContentType::Data, 10)};
    Put(cyclic, 16 + 0x50 + 4, u32{0}); // root's sibling points back at root
    std::ofstream(registered / "duplicate.nca/00", std::ios::binary).write(reinterpret_cast<const char *>(cyclic.data()), cyclic.size());
    firmware.Reload();
    assert(firmware.Version() == 0);
    const auto rejectArchive = [&](const std::vector<u8> &bytes) {
        std::ofstream(registered / "duplicate.nca/00", std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        firmware.Reload();
        assert(firmware.Version() == 0);
    };
    auto childCycle{Archive(0x0100000000000823, vfs::NCAContentType::Data, 10)};
    Put(childCycle, 16 + 0x50 + 8, u32{0});
    rejectArchive(childCycle);
    auto fileCycle{Archive(0x0100000000000823, vfs::NCAContentType::Data, 10)};
    Put(fileCycle, 16 + 0x68 + 4, u32{0});
    rejectArchive(fileCycle);
    auto hugeName{Archive(0x0100000000000823, vfs::NCAContentType::Data, 10)};
    Put(hugeName, 16 + 0x50 + 20, UINT32_MAX);
    rejectArchive(hugeName);
    auto hugeOffset{Archive(0x0100000000000823, vfs::NCAContentType::Data, 10)};
    Put(hugeOffset, 16 + 0x68 + 8, UINT64_MAX);
    rejectArchive(hugeOffset);
    auto outsideTable{Archive(0x0100000000000823, vfs::NCAContentType::Data, 10)};
    Put(outsideTable, 16 + 0x50 + 12, u32{0x1000});
    rejectArchive(outsideTable);
    auto wrappedTable{Archive(0x0100000000000823, vfs::NCAContentType::Data, 10)};
    Put(wrappedTable, 16 + 0x18, UINT64_MAX); // directory metadata table offset
    rejectArchive(wrappedTable);
    std::cout << "NGC content-reader tests passed\n";
}
