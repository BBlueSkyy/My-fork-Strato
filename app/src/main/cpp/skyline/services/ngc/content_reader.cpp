// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <os.h>
#include <vfs/nca.h>
#include <vfs/os_filesystem.h>
#include <vfs/rom_filesystem.h>
#include "content_reader.h"

namespace skyline::service::ngc {
    namespace {
        constexpr u64 DictionaryTitleId{0x0100000000000823};

        template<class T> bool ReadObject(const std::shared_ptr<vfs::Backing> &backing, size_t offset, T &object) {
            const auto [count, error]{backing->ReadWithError(span<u8>{reinterpret_cast<u8 *>(&object), sizeof(object)}, offset)};
            return !error && count == sizeof(object);
        }

        // The general RomFileSystem walker assumes trusted acyclic metadata.
        // Validate this small system archive before handing it to that walker.
        bool ValidateRomFs(const std::shared_ptr<vfs::Backing> &backing) {
            using R = vfs::RomFileSystem;
            if (backing->size < sizeof(R::RomFsHeader))
                return false;
            R::RomFsHeader header{};
            if (!ReadObject(backing, 0, header))
                return false;
            const auto inRange = [](u64 offset, u64 size, u64 limit) {
                return offset <= limit && size <= limit - offset;
            };
            if (header.headerSize != sizeof(header) || header.dataOffset > backing->size ||
                header.dirMetaTableSize > ContentReader::MaxFileSize || header.fileMetaTableSize > ContentReader::MaxFileSize ||
                !inRange(header.dirMetaTableOffset, header.dirMetaTableSize, backing->size) ||
                !inRange(header.fileMetaTableOffset, header.fileMetaTableSize, backing->size))
                return false;

            struct Directory { u32 offset; size_t depth; size_t pathBytes; };
            std::vector<Directory> pending{{0, 0, 0}};
            std::unordered_set<u32> directories, files;
            size_t indexedPathBytes{};
            while (!pending.empty()) {
                const auto current{pending.back()};
                pending.pop_back();
                if (current.offset == constant::RomFsEmptyEntry)
                    continue;
                if (current.depth > 64 || directories.size() + files.size() >= 4096 ||
                    !directories.insert(current.offset).second ||
                    !inRange(current.offset, sizeof(R::RomFsDirectoryEntry), header.dirMetaTableSize))
                    return false;
                R::RomFsDirectoryEntry entry{};
                if (!ReadObject(backing, header.dirMetaTableOffset + current.offset, entry))
                    return false;
                if (entry.nameSize > 0x300 ||
                    !inRange(u64(current.offset) + sizeof(entry), entry.nameSize, header.dirMetaTableSize))
                    return false;
                const auto pathBytes{current.pathBytes + entry.nameSize + 1};
                indexedPathBytes += pathBytes;
                if (pathBytes > 4096 || indexedPathBytes > 8 * 1024 * 1024)
                    return false;
                pending.push_back({entry.siblingOffset, current.depth, current.pathBytes});
                pending.push_back({entry.childOffset, current.depth + 1, pathBytes});
                for (auto offset = entry.fileOffset; offset != constant::RomFsEmptyEntry;) {
                    if (directories.size() + files.size() >= 4096 || !files.insert(offset).second ||
                        !inRange(offset, sizeof(R::RomFsFileEntry), header.fileMetaTableSize))
                        return false;
                    R::RomFsFileEntry file{};
                    if (!ReadObject(backing, header.fileMetaTableOffset + offset, file))
                        return false;
                    indexedPathBytes += pathBytes + file.nameSize;
                    if (file.nameSize > 0x300 || pathBytes + file.nameSize > 4096 ||
                        indexedPathBytes > 8 * 1024 * 1024 ||
                        !inRange(u64(offset) + sizeof(file), file.nameSize, header.fileMetaTableSize) ||
                        !inRange(file.offset, file.size, backing->size - header.dataOffset))
                        return false;
                    offset = file.siblingOffset;
                }
            }
            return true;
        }

        std::shared_ptr<vfs::FileSystem> MountContent(const std::string &publicPath, const std::string &privatePath) {
            // Explicitly supplied extracted RomFS takes precedence, like other
            // user-provided system assets. Do not create this directory on lookup.
            const auto extractedPath{std::filesystem::path(publicPath) / "switch/ngc/0100000000000823"};
            auto [extracted, extractedError]{vfs::OsFileSystem::OpenExisting(extractedPath.string())};
            if (extracted && !extractedError)
                return extracted;

            auto [registered, registeredError]{vfs::OsFileSystem::OpenExisting(
                (std::filesystem::path(publicPath) / "switch/nand/system/Contents/registered").string())};
            if (!registered || registeredError)
                return {};

            auto directory{registered->OpenDirectoryUnchecked("")};
            if (!directory)
                return {};
            auto keys{std::make_shared<crypto::KeyStore>((std::filesystem::path(privatePath) / "keys").string())};
            std::shared_ptr<vfs::Backing> selected;
            for (const auto &entry : directory->Read()) {
                // Registered content is either a flat .nca or an .nca/00 archive.
                if (std::filesystem::path(entry.name).extension() != ".nca")
                    continue;
                try {
                    auto backing{registered->OpenFileUnchecked(entry.type == vfs::Directory::EntryType::File ? entry.name : entry.name + "/00")};
                    if (!backing)
                        continue;
                    vfs::NCA nca{backing, keys};
                    if (nca.header.titleId != DictionaryTitleId || nca.contentType != vfs::NCAContentType::Data || !nca.romFs)
                        continue;
                    // Without CNMT current-content selection, accepting one of two
                    // firmware versions by directory order would be arbitrary.
                    if (selected) {
                        LOGW("NGC: multiple dictionary Data NCAs; use one firmware archive or an extracted RomFS");
                        return {};
                    }
                    selected = nca.romFs;
                } catch (const std::exception &) {
                    // Other titles and encrypted archives without matching keys
                    // must not prevent finding a readable NGC archive.
                }
            }
            if (!selected || !ValidateRomFs(selected))
                return {};
            return std::make_shared<vfs::RomFileSystem>(std::move(selected));
        }

        bool IsFileName(std::string_view name) {
            return !name.empty() && name != "." && name != ".." &&
                name.find_first_of("/\\") == std::string_view::npos &&
                name.find('\0') == std::string_view::npos;
        }
    }

    ContentReader::ContentReader(const DeviceState &state)
        : ContentReader([publicPath = state.os->publicAppFilesPath, privatePath = state.os->privateAppFilesPath] {
            return MountContent(publicPath, privatePath);
        }) {}

    ContentReader::ContentReader(Mount mount) : mount(std::move(mount)) {}

    bool ContentReader::EnsureMounted() {
        if (!attempted) {
            attempted = true;
            try { filesystem = mount(); }
            catch (const std::exception &) { filesystem.reset(); }
        }
        return filesystem != nullptr;
    }

    bool ContentReader::ReadFile(std::string_view name, std::vector<u8> &output) {
        output.clear();
        if (!IsFileName(name) || !EnsureMounted())
            return false;
        try {
            auto file{filesystem->OpenFileUnchecked(std::string(name))};
            if (!file || file->size > MaxFileSize)
                return false;
            std::vector<u8> bytes(file->size);
            const auto [count, error]{file->ReadWithError(span<u8>{bytes.data(), bytes.size()})};
            if (error || count != bytes.size())
                return false;
            output = std::move(bytes);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    bool ContentReader::Reload() {
        const bool wasMounted{filesystem != nullptr};
        filesystem.reset();
        attempted = false;
        // An uninitialized reader can remain unavailable. Losing an existing
        // mount is a reload failure, as opposed to a successful empty dictionary.
        return EnsureMounted() || !wasMounted;
    }

    u32 ContentReader::Version() {
        std::vector<u8> bytes;
        if (!ReadFile("version.dat", bytes) || bytes.size() < sizeof(u32))
            return 0;
        return (u32(bytes[0]) << 24) | (u32(bytes[1]) << 16) | (u32(bytes[2]) << 8) | u32(bytes[3]);
    }
}
