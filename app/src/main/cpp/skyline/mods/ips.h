// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <common.h>

namespace skyline::mods {
    struct IpsWrite {
        u64 offset;
        std::vector<u8> value;
    };

    struct IpsPatch {
        std::filesystem::path path;
        bool ips32{};
        std::vector<IpsWrite> writes;
    };

    namespace ips_detail {
        constexpr size_t NsoHeaderSize{0x100};

        inline std::string Lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        inline bool IsHex(std::string_view value) {
            return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            });
        }

        inline u16 ReadBe16(const u8 *value) {
            return static_cast<u16>((static_cast<u16>(value[0]) << 8) | value[1]);
        }

        inline u32 ReadBe24(const u8 *value) {
            return (static_cast<u32>(value[0]) << 16) |
                   (static_cast<u32>(value[1]) << 8) |
                   static_cast<u32>(value[2]);
        }

        inline u32 ReadBe32(const u8 *value) {
            return (static_cast<u32>(value[0]) << 24) |
                   (static_cast<u32>(value[1]) << 16) |
                   (static_cast<u32>(value[2]) << 8) |
                   static_cast<u32>(value[3]);
        }

        inline bool MatchesBuildId(const std::filesystem::path &path, const std::array<u64, 4> &buildId) {
            const std::string stem{path.stem().string()};
            if (stem.empty() || stem.size() > sizeof(buildId) * 2 || (stem.size() & 1) || !IsHex(stem))
                return false;

            std::array<u8, sizeof(buildId)> parsed{};
            for (size_t source{}, destination{}; source < stem.size(); source += 2, destination++) {
                auto HexNibble = [](char c) -> u8 {
                    if (c >= '0' && c <= '9')
                        return static_cast<u8>(c - '0');
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    return static_cast<u8>(c - 'A' + 10);
                };
                parsed[destination] = static_cast<u8>((HexNibble(stem[source]) << 4) | HexNibble(stem[source + 1]));
            }

            return std::memcmp(parsed.data(), buildId.data(), parsed.size()) == 0;
        }

        inline void AddWrite(std::vector<IpsWrite> &writes, u64 offset, const u8 *data, size_t size) {
            if (!size)
                return;

            if (offset < NsoHeaderSize) {
                const size_t skipped{std::min<size_t>(size, NsoHeaderSize - static_cast<size_t>(offset))};
                offset += skipped;
                data += skipped;
                size -= skipped;
            }
            if (!size)
                return;

            writes.push_back({offset, std::vector<u8>(data, data + size)});
        }

        inline void AddRleWrite(std::vector<IpsWrite> &writes, u64 offset, size_t size, u8 value) {
            if (!size)
                return;

            if (offset < NsoHeaderSize) {
                const size_t skipped{std::min<size_t>(size, NsoHeaderSize - static_cast<size_t>(offset))};
                offset += skipped;
                size -= skipped;
            }
            if (!size)
                return;

            writes.push_back({offset, std::vector<u8>(size, value)});
        }

        inline std::optional<IpsPatch> Parse(const std::filesystem::path &path) {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return std::nullopt;

            std::vector<u8> data{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()
            };
            if (data.size() < 8)
                return std::nullopt;

            constexpr std::string_view IpsMagic{"PATCH"};
            constexpr std::string_view Ips32Magic{"IPS32"};
            constexpr std::string_view IpsFooter{"EOF"};
            constexpr std::string_view Ips32Footer{"EEOF"};

            bool ips32{};
            if (std::memcmp(data.data(), IpsMagic.data(), IpsMagic.size()) == 0) {
                ips32 = false;
            } else if (std::memcmp(data.data(), Ips32Magic.data(), Ips32Magic.size()) == 0) {
                ips32 = true;
            } else {
                return std::nullopt;
            }

            const auto footer{ips32 ? Ips32Footer : IpsFooter};
            const size_t offsetSize{ips32 ? 4U : 3U};
            size_t cursor{5};
            std::vector<IpsWrite> writes;

            while (true) {
                if (cursor + footer.size() <= data.size() &&
                    std::memcmp(data.data() + cursor, footer.data(), footer.size()) == 0) {
                    return IpsPatch{path, ips32, std::move(writes)};
                }

                if (cursor + offsetSize + 2 > data.size())
                    return std::nullopt;

                const u64 offset{ips32 ? ReadBe32(data.data() + cursor) : ReadBe24(data.data() + cursor)};
                cursor += offsetSize;

                const u16 patchSize{ReadBe16(data.data() + cursor)};
                cursor += 2;

                if (patchSize == 0) {
                    if (cursor + 3 > data.size())
                        return std::nullopt;
                    const u16 rleSize{ReadBe16(data.data() + cursor)};
                    cursor += 2;
                    const u8 value{data[cursor++]};
                    AddRleWrite(writes, offset, rleSize, value);
                } else {
                    if (cursor + patchSize > data.size())
                        return std::nullopt;
                    AddWrite(writes, offset, data.data() + cursor, patchSize);
                    cursor += patchSize;
                }
            }
        }

        inline std::optional<std::filesystem::path> FindExeFsPatches(const std::filesystem::path &modDirectory) {
            std::error_code error;
            for (std::filesystem::directory_iterator it(modDirectory, error), end; !error && it != end; it.increment(error)) {
                if (it->is_directory(error) && !error && Lower(it->path().filename().string()) == "exefs_patches")
                    return it->path();
            }
            return std::nullopt;
        }

        inline std::vector<std::filesystem::path> CollectIpsFiles(const std::filesystem::path &directory, const std::array<u64, 4> &buildId) {
            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
                if (!it->is_regular_file(error) || error)
                    continue;

                const auto &path{it->path()};
                if (Lower(path.extension().string()) == ".ips" && MatchesBuildId(path, buildId))
                    files.push_back(path);
            }
            std::sort(files.begin(), files.end());
            return files;
        }
    }

    inline std::vector<IpsPatch> CollectIpsPatches(const std::string &publicAppFilesPath, u64 titleId, const std::array<u64, 4> &buildId) {
        const auto root{std::filesystem::path(publicAppFilesPath) / "switch" / "load" / fmt::format("{:016X}", titleId)};
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error)
            return {};

        std::vector<std::filesystem::path> modDirectories;
        for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_directory(error) || error)
                continue;

            const auto path{it->path()};
            if (path.filename().string().starts_with('.'))
                continue;

            error.clear();
            const bool disabled{std::filesystem::exists(path / ".disabled", error)};
            if (error) {
                error.clear();
                continue;
            }
            if (!disabled)
                modDirectories.push_back(path);
        }
        std::sort(modDirectories.begin(), modDirectories.end());

        std::vector<std::filesystem::path> files;
        for (const auto &modDirectory : modDirectories) {
            const auto patchesDirectory{ips_detail::FindExeFsPatches(modDirectory)};
            if (!patchesDirectory)
                continue;

            auto modFiles{ips_detail::CollectIpsFiles(*patchesDirectory, buildId)};
            files.insert(files.end(), modFiles.begin(), modFiles.end());
        }

        std::vector<IpsPatch> patches;
        for (auto file{files.rbegin()}; file != files.rend(); ++file) {
            auto patch{ips_detail::Parse(*file)};
            if (!patch) {
                LOGW("IPS: failed to parse '{}'", file->string());
                continue;
            }
            if (!patch->writes.empty())
                patches.push_back(std::move(*patch));
        }
        return patches;
    }
}
