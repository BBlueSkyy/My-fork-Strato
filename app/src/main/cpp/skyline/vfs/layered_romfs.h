// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "os_filesystem.h"
#include "rom_filesystem.h"

namespace skyline::vfs {
    /**
     * @brief Rebuilds RomFS metadata while keeping file data in the original or host backings.
     * Earlier mod directories have priority over later ones and the base RomFS.
     */
    class LayeredRomFsBacking final : public Backing {
      private:
        static constexpr u32 EmptyEntry{0xFFFFFFFF};
        static constexpr size_t FilePartitionOffset{0x200};
        static constexpr size_t MaxPathLength{0x301};

        struct FileSource {
            std::shared_ptr<Backing> backing;
            size_t offset;
            size_t size;
        };

        struct DirectoryNode {
            std::string path;
            std::string name;
            std::string parentPath;
            u32 entryOffset{};
            u32 siblingOffset{EmptyEntry};
            u32 childOffset{EmptyEntry};
            u32 fileOffset{EmptyEntry};
        };

        struct FileNode {
            std::string path;
            std::string name;
            std::string parentPath;
            FileSource source;
            u32 entryOffset{};
            u32 siblingOffset{EmptyEntry};
            u64 dataOffset{};
        };

        struct Segment {
            size_t offset;
            size_t size;
            std::shared_ptr<Backing> backing;
            size_t sourceOffset;
        };

        std::vector<u8> headerData;
        std::vector<u8> metadata;
        size_t metadataOffset{};
        std::vector<Segment> segments;

        static size_t AlignUp(size_t value, size_t alignment) {
            if (value > std::numeric_limits<size_t>::max() - (alignment - 1))
                throw exception("RomFS size overflow");
            return (value + alignment - 1) & ~(alignment - 1);
        }

        static u64 GetHashTableCount(u64 entries) {
            if (entries < 3)
                return 3;
            if (entries < 19)
                return entries | 1;

            u64 count{entries};
            while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 ||
                   count % 11 == 0 || count % 13 == 0 || count % 17 == 0)
                count++;
            return count;
        }

        static u32 PathHash(u32 parentOffset, std::string_view name) {
            u32 hash{parentOffset ^ 123456789U};
            for (unsigned char c : name) {
                hash = (hash >> 5) | (hash << 27);
                hash ^= c;
            }
            return hash;
        }

        static std::string ParentPath(std::string_view path) {
            const auto separator{path.find_last_of('/')};
            if (separator == std::string_view::npos)
                return {};
            return std::string(path.substr(0, separator));
        }

        static std::string BaseName(std::string_view path) {
            const auto separator{path.find_last_of('/')};
            return std::string(separator == std::string_view::npos ? path : path.substr(separator + 1));
        }

        static void AddParentDirectories(std::set<std::string> &directories, std::string path) {
            while (true) {
                path = ParentPath(path);
                directories.emplace(path);
                if (path.empty())
                    return;
            }
        }

        static void AddModLayer(const std::filesystem::path &root,
                                std::map<std::string, FileSource> &files,
                                std::set<std::string> &directories,
                                std::unordered_set<std::string> &claimedFiles) {
            OsFileSystem filesystem(root.string());
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
            const std::filesystem::recursive_directory_iterator end;

            while (!error && iterator != end) {
                const auto &entry{*iterator};
                auto relative{std::filesystem::relative(entry.path(), root, error)};
                if (error)
                    break;

                const auto path{relative.generic_string()};
                if (entry.is_directory(error)) {
                    if (error)
                        break;
                    directories.emplace(path);
                    AddParentDirectories(directories, path);
                } else if (entry.is_regular_file(error)) {
                    if (error)
                        break;
                    AddParentDirectories(directories, path);
                    if (claimedFiles.emplace(path).second) {
                        auto backing{filesystem.OpenFile(path)};
                        files[path] = FileSource{backing, 0, backing->size};
                    }
                }

                iterator.increment(error);
            }

            if (error)
                throw exception("Failed to enumerate RomFS mod '{}': {}", root.string(), error.message());
        }

        void Build(const std::shared_ptr<Backing> &baseBacking, const std::vector<std::filesystem::path> &modDirectories) {
            if (!baseBacking)
                throw exception("Cannot layer a null RomFS backing");

            RomFileSystem baseRomFs(baseBacking);
            std::map<std::string, FileSource> effectiveFiles;
            std::set<std::string> effectiveDirectories;
            effectiveDirectories.emplace("");

            for (const auto &[path, entry] : baseRomFs.directoryMap)
                effectiveDirectories.emplace(path);

            for (const auto &[path, entry] : baseRomFs.fileMap) {
                if (entry.size > std::numeric_limits<size_t>::max())
                    throw exception("RomFS file is too large");
                effectiveFiles.emplace(path, FileSource{
                    baseBacking,
                    static_cast<size_t>(baseRomFs.header.dataOffset + entry.offset),
                    static_cast<size_t>(entry.size),
                });
            }

            std::unordered_set<std::string> claimedFiles;
            for (const auto &directory : modDirectories)
                AddModLayer(directory, effectiveFiles, effectiveDirectories, claimedFiles);

            std::vector<DirectoryNode> directories;
            directories.reserve(effectiveDirectories.size());
            for (const auto &path : effectiveDirectories) {
                const auto name{BaseName(path)};
                if (path.size() + 1 >= MaxPathLength)
                    throw exception("RomFS directory path is too long: {}", path);
                directories.push_back(DirectoryNode{path, name, ParentPath(path)});
            }

            std::vector<FileNode> files;
            files.reserve(effectiveFiles.size());
            for (const auto &[path, source] : effectiveFiles) {
                if (path.size() + 1 >= MaxPathLength)
                    throw exception("RomFS file path is too long: {}", path);
                files.push_back(FileNode{path, BaseName(path), ParentPath(path), source});
            }

            std::unordered_map<std::string, size_t> directoryIndices;
            directoryIndices.reserve(directories.size());
            for (size_t index{}; index < directories.size(); index++)
                directoryIndices.emplace(directories[index].path, index);

            size_t directoryTableSize{};
            for (auto &directory : directories) {
                if (directoryTableSize > std::numeric_limits<u32>::max())
                    throw exception("RomFS directory table is too large");
                directory.entryOffset = static_cast<u32>(directoryTableSize);
                directoryTableSize += sizeof(RomFileSystem::RomFsDirectoryEntry) + AlignUp(directory.name.size(), 4);
            }

            size_t fileTableSize{};
            size_t filePartitionSize{};
            for (auto &file : files) {
                if (fileTableSize > std::numeric_limits<u32>::max())
                    throw exception("RomFS file table is too large");
                file.entryOffset = static_cast<u32>(fileTableSize);
                fileTableSize += sizeof(RomFileSystem::RomFsFileEntry) + AlignUp(file.name.size(), 4);

                filePartitionSize = AlignUp(filePartitionSize, 16);
                file.dataOffset = filePartitionSize;
                if (file.source.size > std::numeric_limits<size_t>::max() - filePartitionSize)
                    throw exception("RomFS file partition is too large");
                filePartitionSize += file.source.size;
            }

            std::unordered_map<std::string, std::vector<size_t>> childDirectories;
            std::unordered_map<std::string, std::vector<size_t>> childFiles;
            for (size_t index{1}; index < directories.size(); index++) {
                if (!directoryIndices.contains(directories[index].parentPath))
                    throw exception("RomFS directory has no parent: {}", directories[index].path);
                childDirectories[directories[index].parentPath].push_back(index);
            }
            for (size_t index{}; index < files.size(); index++) {
                if (!directoryIndices.contains(files[index].parentPath))
                    throw exception("RomFS file has no parent: {}", files[index].path);
                childFiles[files[index].parentPath].push_back(index);
            }

            for (auto &[parentPath, children] : childDirectories) {
                auto &parent{directories[directoryIndices.at(parentPath)]};
                parent.childOffset = directories[children.front()].entryOffset;
                for (size_t index{}; index + 1 < children.size(); index++)
                    directories[children[index]].siblingOffset = directories[children[index + 1]].entryOffset;
            }
            for (auto &[parentPath, children] : childFiles) {
                auto &parent{directories[directoryIndices.at(parentPath)]};
                parent.fileOffset = files[children.front()].entryOffset;
                for (size_t index{}; index + 1 < children.size(); index++)
                    files[children[index]].siblingOffset = files[children[index + 1]].entryOffset;
            }

            const size_t directoryHashCount{static_cast<size_t>(GetHashTableCount(directories.size()))};
            const size_t fileHashCount{static_cast<size_t>(GetHashTableCount(files.size()))};
            std::vector<u32> directoryHashes(directoryHashCount, EmptyEntry);
            std::vector<u32> fileHashes(fileHashCount, EmptyEntry);
            std::vector<u8> directoryTable(directoryTableSize);
            std::vector<u8> fileTable(fileTableSize);

            for (const auto &directory : directories) {
                const u32 parentOffset{directory.path.empty() ? 0U : directories[directoryIndices.at(directory.parentPath)].entryOffset};
                RomFileSystem::RomFsDirectoryEntry entry{
                    .parentOffset = parentOffset,
                    .siblingOffset = directory.siblingOffset,
                    .childOffset = directory.childOffset,
                    .fileOffset = directory.fileOffset,
                    .hash = 0,
                    .nameSize = static_cast<u32>(directory.name.size()),
                };
                const auto hash{PathHash(parentOffset, directory.name)};
                entry.hash = directoryHashes[hash % directoryHashCount];
                directoryHashes[hash % directoryHashCount] = directory.entryOffset;

                std::memcpy(directoryTable.data() + directory.entryOffset, &entry, sizeof(entry));
                if (!directory.name.empty())
                    std::memcpy(directoryTable.data() + directory.entryOffset + sizeof(entry), directory.name.data(), directory.name.size());
            }

            segments.reserve(files.size());
            for (const auto &file : files) {
                const u32 parentOffset{directories[directoryIndices.at(file.parentPath)].entryOffset};
                RomFileSystem::RomFsFileEntry entry{
                    .parentOffset = parentOffset,
                    .siblingOffset = file.siblingOffset,
                    .offset = file.dataOffset,
                    .size = file.source.size,
                    .hash = 0,
                    .nameSize = static_cast<u32>(file.name.size()),
                };
                const auto hash{PathHash(parentOffset, file.name)};
                entry.hash = fileHashes[hash % fileHashCount];
                fileHashes[hash % fileHashCount] = file.entryOffset;

                std::memcpy(fileTable.data() + file.entryOffset, &entry, sizeof(entry));
                if (!file.name.empty())
                    std::memcpy(fileTable.data() + file.entryOffset + sizeof(entry), file.name.data(), file.name.size());

                if (file.source.size) {
                    segments.push_back(Segment{
                        FilePartitionOffset + static_cast<size_t>(file.dataOffset),
                        file.source.size,
                        file.source.backing,
                        file.source.offset,
                    });
                }
            }

            RomFileSystem::RomFsHeader header{};
            header.headerSize = sizeof(header);
            header.dataOffset = FilePartitionOffset;
            header.dirHashTableOffset = AlignUp(FilePartitionOffset + filePartitionSize, 4);
            header.dirHashTableSize = directoryHashes.size() * sizeof(u32);
            header.dirMetaTableOffset = header.dirHashTableOffset + header.dirHashTableSize;
            header.dirMetaTableSize = directoryTable.size();
            header.fileHashTableOffset = header.dirMetaTableOffset + header.dirMetaTableSize;
            header.fileHashTableSize = fileHashes.size() * sizeof(u32);
            header.fileMetaTableOffset = header.fileHashTableOffset + header.fileHashTableSize;
            header.fileMetaTableSize = fileTable.size();

            headerData.resize(sizeof(header));
            std::memcpy(headerData.data(), &header, sizeof(header));

            metadataOffset = static_cast<size_t>(header.dirHashTableOffset);
            metadata.reserve(static_cast<size_t>(header.fileMetaTableOffset + header.fileMetaTableSize - header.dirHashTableOffset));
            const auto append = [this](const void *data, size_t dataSize) {
                const auto *bytes{static_cast<const u8 *>(data)};
                metadata.insert(metadata.end(), bytes, bytes + dataSize);
            };
            append(directoryHashes.data(), directoryHashes.size() * sizeof(u32));
            append(directoryTable.data(), directoryTable.size());
            append(fileHashes.data(), fileHashes.size() * sizeof(u32));
            append(fileTable.data(), fileTable.size());

            size = static_cast<size_t>(header.fileMetaTableOffset + header.fileMetaTableSize);
        }

        size_t ReadImpl(span<u8> output, size_t offset) override {
            if (offset >= size)
                return 0;

            const size_t readSize{std::min(output.size(), size - offset)};
            std::fill(output.begin(), output.begin() + readSize, 0);
            const size_t end{offset + readSize};

            const auto copyMemory = [&](const std::vector<u8> &source, size_t sourceOffset) {
                const size_t sourceEnd{sourceOffset + source.size()};
                const size_t begin{std::max(offset, sourceOffset)};
                const size_t overlapEnd{std::min(end, sourceEnd)};
                if (begin < overlapEnd)
                    std::memcpy(output.data() + (begin - offset), source.data() + (begin - sourceOffset), overlapEnd - begin);
            };

            copyMemory(headerData, 0);
            copyMemory(metadata, metadataOffset);

            auto segment = std::lower_bound(segments.begin(), segments.end(), offset, [](const Segment &entry, size_t value) {
                return entry.offset + entry.size <= value;
            });
            for (; segment != segments.end() && segment->offset < end; ++segment) {
                const size_t begin{std::max(offset, segment->offset)};
                const size_t segmentEnd{segment->offset + segment->size};
                const size_t overlapEnd{std::min(end, segmentEnd)};
                if (begin >= overlapEnd)
                    continue;

                const size_t amount{overlapEnd - begin};
                segment->backing->Read(span<u8>(output.data() + (begin - offset), amount), segment->sourceOffset + (begin - segment->offset));
            }

            return readSize;
        }

      public:
        LayeredRomFsBacking(std::shared_ptr<Backing> baseBacking, std::vector<std::filesystem::path> modDirectories)
            : Backing({true, false, false}, 0) {
            Build(baseBacking, modDirectories);
        }
    };
}
