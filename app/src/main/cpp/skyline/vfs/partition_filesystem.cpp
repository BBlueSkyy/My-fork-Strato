// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "region_backing.h"
#include "partition_filesystem.h"

namespace skyline::vfs {
    PartitionFileSystem::PartitionFileSystem(const std::shared_ptr<Backing> &backing) : FileSystem(), backing(backing) {
        if (backing->size < sizeof(header) ||
            backing->Read(span<u8>(reinterpret_cast<u8 *>(&header), sizeof(header))) != sizeof(header))
            throw exception("Truncated partition filesystem header");

        if (header.magic == util::MakeMagic<u32>("PFS0"))
            hashed = false;
        else if (header.magic == util::MakeMagic<u32>("HFS0"))
            hashed = true;
        else
            throw exception("Invalid filesystem magic: {}", header.magic);

        const size_t entrySize{hashed ? sizeof(HashedFileEntry) : sizeof(PartitionFileEntry)};
        if (header.numFiles > (backing->size - sizeof(FsHeader)) / entrySize)
            throw exception("Partition filesystem entry table exceeds backing");
        const size_t stringTableOffset{sizeof(FsHeader) + (header.numFiles * entrySize)};
        if (header.stringTableSize > backing->size - stringTableOffset)
            throw exception("Partition filesystem string table exceeds backing");
        fileDataOffset = stringTableOffset + header.stringTableSize;

        std::vector<char> stringTable(header.stringTableSize);
        if (backing->Read(span(stringTable), stringTableOffset) != stringTable.size())
            throw exception("Short partition filesystem string table read");

        const size_t dataSize{backing->size - fileDataOffset};
        for (u32 index{}; index < header.numFiles; ++index) {
            PartitionFileEntry entry{};
            const size_t entryOffset{sizeof(FsHeader) + index * entrySize};
            if (backing->Read(span<u8>(reinterpret_cast<u8 *>(&entry), sizeof(entry)), entryOffset) != sizeof(entry))
                throw exception("Short partition filesystem entry read");
            if (entry.offset > dataSize || entry.size > dataSize - entry.offset)
                throw exception("Partition filesystem file {} exceeds backing", index);
            if (hashed && entry.hashedSize > entry.size)
                throw exception("HFS0 hashed prefix exceeds file {}", index);
            if (entry.stringTableOffset >= stringTable.size())
                throw exception("Partition filesystem name offset exceeds string table");
            const auto nameStart{stringTable.begin() + entry.stringTableOffset};
            const auto nameEnd{std::find(nameStart, stringTable.end(), '\0')};
            if (nameEnd == stringTable.end())
                throw exception("Unterminated partition filesystem name");

            std::string name(nameStart, nameEnd);
            fileMap.emplace(name, entry);
        }
    }

    std::shared_ptr<Backing> PartitionFileSystem::OpenFileImpl(const std::string &path, Backing::Mode mode) {
        try {
            auto &entry{fileMap.at(path)};
            return std::make_shared<RegionBacking>(backing, fileDataOffset + entry.offset, entry.size, mode);
        } catch (std::out_of_range &e) {
            return nullptr;
        }
    }

    std::optional<Directory::EntryType> PartitionFileSystem::GetEntryTypeImpl(const std::string &path) {
        if (fileMap.count(path))
            return Directory::EntryType::File;

        return std::nullopt;
    }

    std::shared_ptr<Directory> PartitionFileSystem::OpenDirectoryImpl(const std::string &path, Directory::ListMode listMode) {
        // PFS doesn't have directories
        if (!path.empty())
            return nullptr;

        std::vector<Directory::Entry> fileList;
        for (const auto &file : fileMap)
            fileList.emplace_back(Directory::Entry{file.first, Directory::EntryType::File, file.second.size});

        return std::make_shared<PartitionFileSystemDirectory>(fileList, listMode);
    }

    PartitionFileSystemDirectory::PartitionFileSystemDirectory(std::vector<Entry> fileList, ListMode listMode) : Directory(listMode), fileList(std::move(fileList)) {}

    std::vector<Directory::Entry> PartitionFileSystemDirectory::Read() {
        if (listMode.file)
            return fileList;
        else
            return std::vector<Entry>();
    }
}
