// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace skyline::loader::homebrew {
    /**
     * Resolve an app-owned host SD card path to the Homebrew ABI's sdmc:/ path.
     *
     * When an NRO is started through an external Android SAF provider we cannot
     * promise its sibling data is accessible to the guest. In that case leave
     * argv[0] unset and let the user import its whole folder with the installer.
     */
    inline std::optional<std::string> ResolveGuestNroPath(std::string_view hostPath,
                                                          std::string_view appFilesPath) {
        if (hostPath.empty() || appFilesPath.empty())
            return std::nullopt;

        std::string root(appFilesPath);
        while (!root.empty() && root.back() == '/')
            root.pop_back();
        if (root.empty())
            return std::nullopt;
        root += "/switch/sdmc/";

        if (hostPath.size() <= root.size() || hostPath.substr(0, root.size()) != root)
            return std::nullopt;

        const std::string_view relative{hostPath.substr(root.size())};
        // Prevent an argv entry from accidentally targeting another device,
        // escaping the emulated SD root, or being split by the libnx quote parser.
        if (relative.find_first_of("\\\"\r\n:") != std::string_view::npos)
            return std::nullopt;

        size_t start{};
        while (start < relative.size()) {
            const auto end{relative.find('/', start)};
            const auto part{relative.substr(start, end == std::string_view::npos
                                                      ? relative.size() - start : end - start)};
            if (part.empty() || part == "." || part == "..")
                return std::nullopt;
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        if (relative.back() == '/' || relative.find('\0') != std::string_view::npos)
            return std::nullopt;
        return "sdmc:/" + std::string(relative);
    }
}
