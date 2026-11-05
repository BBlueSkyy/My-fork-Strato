// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skyline::service::ngc::dictionary {
    using FileReader = std::function<std::optional<std::vector<std::uint8_t>>(std::string_view)>;
    enum class Error { Success, Unavailable, Malformed, Unsupported, InvalidUtf8 };
    struct Options {
        std::uint32_t skipAtSignCheck{};
        std::uint32_t maskMode{};
        std::uint32_t flags{};
        std::uint32_t systemRegionMask{};
        std::uint32_t reserved{};
    };
    struct Result {
        Error error{Error::Success};
        std::uint32_t value{};
    };
    // Consumes the gzip-compressed, unmodified ac_* files from system title
    // 0100000000000823. No dictionary data is bundled. ASCII input is supported;
    // Unicode and canonicalized masking return Unsupported rather than a guess.
    class Filter {
        FileReader readFile;
      public:
        explicit Filter(FileReader reader);
        Result Check(std::string_view word, std::uint32_t regionMask, Options options) const;
        // Filters the first 512 characters; preserves bytes after the first NUL.
        // Commits the output only on success.
        Result Mask(std::string &text, std::uint32_t regionMask, Options options) const;
    };
}
