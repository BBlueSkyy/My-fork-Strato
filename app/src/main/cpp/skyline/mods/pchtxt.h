// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <common.h>

namespace skyline::mods {
    struct PchtxtWrite {
        u64 offset;
        std::vector<u8> value;
    };

    struct PchtxtPatch {
        std::filesystem::path path;
        std::vector<PchtxtWrite> writes;
    };

    namespace detail {
        inline void Trim(std::string &value) {
            auto first{std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); })};
            auto last{std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base()};
            if (first >= last) {
                value.clear();
                return;
            }
            value = std::string(first, last);
        }

        inline std::string Lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        inline bool IsTag(std::string_view line, std::string_view tag) {
            return line == tag || (line.size() > tag.size() && line.substr(0, tag.size()) == tag && std::isspace(static_cast<unsigned char>(line[tag.size()])));
        }

        inline std::string StripComment(const std::string &line) {
            bool quoted{};
            bool escaped{};
            for (size_t i{}; i < line.size(); i++) {
                const char c{line[i]};
                if (quoted && c == '\\' && !escaped) {
                    escaped = true;
                    continue;
                }
                if (c == '"' && !escaped)
                    quoted = !quoted;
                else if (c == '/' && !quoted)
                    return line.substr(0, i);
                escaped = false;
            }
            return line;
        }

        inline bool IsHex(std::string_view value) {
            return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
        }

        inline std::optional<std::string> NormalizeBuildId(std::string value) {
            Trim(value);
            if (value.empty() || value.size() > 64 || !IsHex(value))
                return std::nullopt;

            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            value.resize(64, '0');
            return value;
        }

        inline std::optional<std::vector<u8>> ParseValue(const std::string &source, bool bigEndian) {
            std::string value{source};
            Trim(value);
            if (value.empty())
                return std::nullopt;

            if (value.front() == '"') {
                std::vector<u8> output;
                bool escaped{};
                bool closed{};
                for (size_t i{1}; i < value.size(); i++) {
                    char c{value[i]};
                    if (!escaped && c == '"') {
                        closed = true;
                        break;
                    }
                    if (!escaped && c == '\\') {
                        escaped = true;
                        continue;
                    }
                    if (escaped) {
                        switch (c) {
                            case 'a': c = '\a'; break;
                            case 'b': c = '\b'; break;
                            case 'f': c = '\f'; break;
                            case 'n': c = '\n'; break;
                            case 'r': c = '\r'; break;
                            case 't': c = '\t'; break;
                            case 'v': c = '\v'; break;
                            default: break;
                        }
                        escaped = false;
                    }
                    output.push_back(static_cast<u8>(c));
                }
                if (!closed)
                    return std::nullopt;
                output.push_back(0);
                return output;
            }

            std::vector<u8> output;
            std::istringstream stream(value);
            std::string token;
            while (stream >> token) {
                if ((token.size() & 1) || !IsHex(token))
                    return std::nullopt;

                std::vector<u8> bytes;
                bytes.reserve(token.size() / 2);
                for (size_t i{}; i < token.size(); i += 2)
                    bytes.push_back(static_cast<u8>(std::stoul(token.substr(i, 2), nullptr, 16)));
                if (bigEndian)
                    std::reverse(bytes.begin(), bytes.end());
                output.insert(output.end(), bytes.begin(), bytes.end());
            }
            return output.empty() ? std::nullopt : std::optional<std::vector<u8>>{std::move(output)};
        }

        inline std::optional<std::vector<PchtxtWrite>> Parse(const std::filesystem::path &path, const std::string &buildId) {
            std::ifstream input(path);
            if (!input)
                return std::nullopt;

            std::vector<PchtxtWrite> writes;
            std::string currentBuildId;
            i64 offsetShift{};
            bool currentTargetNso{true};
            bool bigEndian{};
            bool accepting{};
            bool enabled{};
            bool binary{true};
            std::string line;

            while (std::getline(input, line)) {
                Trim(line);
                if (line.empty())
                    continue;

                line = StripComment(line);
                Trim(line);
                if (line.empty())
                    continue;

                const auto lower{Lower(line)};
                if (lower == "@stop")
                    break;

                if (lower.starts_with("@nsobid-")) {
                    auto normalized{NormalizeBuildId(line.substr(std::string("@nsobid-").size()))};
                    if (!normalized)
                        return std::nullopt;
                    currentBuildId = *normalized;
                    currentTargetNso = true;
                    accepting = false;
                    continue;
                }

                if (IsTag(lower, "@flag")) {
                    std::string content{line.substr(5)};
                    Trim(content);
                    std::istringstream flags(content);
                    std::string type;
                    flags >> type;
                    type = Lower(type);

                    std::string value;
                    std::getline(flags, value);
                    Trim(value);

                    if (type == "nsobid" || type == "nrobid") {
                        auto normalized{NormalizeBuildId(value)};
                        if (!normalized)
                            return std::nullopt;
                        currentBuildId = *normalized;
                        currentTargetNso = type == "nsobid";
                        accepting = false;
                    } else if (type == "offset_shift") {
                        try {
                            offsetShift = std::stoll(value, nullptr, 0);
                        } catch (...) {
                            return std::nullopt;
                        }
                    } else if (type == "be") {
                        bigEndian = true;
                    } else if (type == "le") {
                        bigEndian = false;
                    }
                    continue;
                }

                const bool enabledTag{IsTag(lower, "@enabled")};
                const bool disabledTag{IsTag(lower, "@disabled")};
                if (enabledTag || disabledTag) {
                    if (currentBuildId.empty())
                        return std::nullopt;

                    const size_t tagSize{enabledTag ? std::string("@enabled").size() : std::string("@disabled").size()};
                    std::string type{lower.substr(tagSize)};
                    Trim(type);
                    if (auto space{type.find_first_of(" \t")}; space != std::string::npos)
                        type.resize(space);

                    accepting = true;
                    enabled = enabledTag;
                    binary = type != "heap" && type != "ams";
                    continue;
                }

                if (line.front() == '[') {
                    accepting = false;
                    continue;
                }
                if (line.front() == '#' || line.front() == '/')
                    continue;
                if (!accepting || !enabled || !binary || !currentTargetNso || currentBuildId != buildId)
                    continue;

                std::istringstream values(line);
                std::string offsetString;
                values >> offsetString;
                if (offsetString.size() > 8 || !IsHex(offsetString))
                    return std::nullopt;

                u64 offset;
                try {
                    offset = std::stoull(offsetString, nullptr, 16);
                } catch (...) {
                    return std::nullopt;
                }

                if (offsetShift >= 0) {
                    const auto shift{static_cast<u64>(offsetShift)};
                    if (offset > std::numeric_limits<u64>::max() - shift)
                        return std::nullopt;
                    offset += shift;
                } else {
                    const auto shift{static_cast<u64>(-(offsetShift + 1)) + 1};
                    if (offset < shift)
                        return std::nullopt;
                    offset -= shift;
                }

                std::string value;
                std::getline(values, value);
                auto parsedValue{ParseValue(value, bigEndian)};
                if (!parsedValue)
                    return std::nullopt;
                writes.push_back({offset, std::move(*parsedValue)});
            }

            return writes;
        }

        inline std::optional<std::filesystem::path> FindExeFs(const std::filesystem::path &modDirectory) {
            std::error_code error;
            for (std::filesystem::directory_iterator it(modDirectory, error), end; !error && it != end; it.increment(error)) {
                if (it->is_directory(error) && !error && Lower(it->path().filename().string()) == "exefs")
                    return it->path();
            }
            return std::nullopt;
        }
    }

    inline std::string FormatBuildId(const std::array<u64, 4> &buildId) {
        constexpr char Hex[]{"0123456789ABCDEF"};
        const auto *bytes{reinterpret_cast<const u8 *>(buildId.data())};
        std::string output;
        output.reserve(64);
        for (size_t i{}; i < sizeof(buildId); i++) {
            output.push_back(Hex[bytes[i] >> 4]);
            output.push_back(Hex[bytes[i] & 0xF]);
        }
        return output;
    }

    inline std::vector<PchtxtPatch> CollectPchtxtPatches(const std::string &publicAppFilesPath, u64 titleId, const std::array<u64, 4> &buildId) {
        const auto root{std::filesystem::path(publicAppFilesPath) / "switch" / "load" / fmt::format("{:016X}", titleId)};
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error)
            return {};

        std::vector<std::filesystem::path> modDirectories;
        for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            if (it->is_directory(error) && !error)
                modDirectories.push_back(it->path());
        }
        std::sort(modDirectories.begin(), modDirectories.end());

        std::vector<std::filesystem::path> files;
        for (const auto &modDirectory : modDirectories) {
            const auto exefs{detail::FindExeFs(modDirectory)};
            if (!exefs)
                continue;

            std::vector<std::filesystem::path> modFiles;
            error.clear();
            for (std::filesystem::directory_iterator it(*exefs, error), end; !error && it != end; it.increment(error)) {
                if (it->is_regular_file(error) && !error && detail::Lower(it->path().extension().string()) == ".pchtxt")
                    modFiles.push_back(it->path());
            }
            std::sort(modFiles.begin(), modFiles.end());
            files.insert(files.end(), modFiles.begin(), modFiles.end());
        }

        const auto formattedBuildId{FormatBuildId(buildId)};
        std::vector<PchtxtPatch> patches;
        for (auto file{files.rbegin()}; file != files.rend(); ++file) {
            auto writes{detail::Parse(*file, formattedBuildId)};
            if (!writes) {
                LOGW("PCHTXT: failed to parse '{}'", file->string());
                continue;
            }
            if (!writes->empty())
                patches.push_back({*file, std::move(*writes)});
        }
        return patches;
    }
}
