// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <vfs/os_filesystem.h>
#include "key_store.h"

namespace skyline::crypto {
    KeyStore::KeyStore(const std::string &rootPath) {
        vfs::OsFileSystem root(rootPath);
        if (root.FileExists("title.keys"))
            ReadPairs(root.OpenFile("title.keys"), &KeyStore::PopulateTitleKeys);
        if (root.FileExists("prod.keys"))
            ReadPairs(root.OpenFile("prod.keys"), &KeyStore::PopulateKeys);
    }

    void KeyStore::ReadPairs(const std::shared_ptr<vfs::Backing> &backing, ReadPairsCallback callback) {
        std::vector<char> fileContent(backing->size);
        backing->Read(span(fileContent));

        if (fileContent.empty())
            return;

        auto trim{[](std::string_view text) {
            const auto first{text.find_first_not_of(" \t\r\f\v")};
            if (first == std::string_view::npos)
                return std::string_view{};
            const auto last{text.find_last_not_of(" \t\r\f\v")};
            return text.substr(first, last - first + 1);
        }};

        std::string_view remaining(fileContent.data(), fileContent.size());
        while (!remaining.empty()) {
            const auto lineEnd{remaining.find('\n')};
            const auto line{trim(remaining.substr(0, lineEnd))};
            remaining = lineEnd == std::string_view::npos ? std::string_view{} : remaining.substr(lineEnd + 1);
            if (line.empty() || line.front() == '#' || line.front() == ';')
                continue;

            const auto separator{line.find('=')};
            if (separator == std::string_view::npos)
                throw exception("Invalid key file");

            (this->*callback)(trim(line.substr(0, separator)), trim(line.substr(separator + 1)));
        }
    }

    void KeyStore::PopulateTitleKeys(std::string_view keyName, std::string_view value) {
        Key128 key{util::HexStringToArray<16>(keyName)};
        Key128 valueArray{util::HexStringToArray<16>(value)};
        titleKeys.emplace(key, valueArray);
    }

    void KeyStore::PopulateTitleKey(Key128 keyName, Key128 value) {
        if (!titleKeys.contains(keyName))
            titleKeys.emplace(keyName, value);
    }

    void KeyStore::PopulateKeys(std::string_view keyName, std::string_view value) {
        {
            auto it{key256Names.find(keyName)};
            if (it != key256Names.end()) {
                it->second = headerKey = util::HexStringToArray<32>(value);
                return;
            }
        }

        if (keyName.size() > 2) {
            auto it{indexedKey128Names.find(keyName.substr(0, keyName.size() - 2))};
            if (it != indexedKey128Names.end()) {
                const auto suffix{keyName.substr(it->first.size())};
                const size_t index{static_cast<size_t>(util::HexDigitToNibble(suffix[0])) * 16 + util::HexDigitToNibble(suffix[1])};
                // Ignore revisions beyond the supported array without indexing it.
                if (index >= it->second.size())
                    return;
                it->second[index] = util::HexStringToArray<16>(value);
            }
        }
    }
}
