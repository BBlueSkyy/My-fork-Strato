// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skyline::service::jit {
    // Private sysmodule virtual addresses; these never refer to host pointers.
    class PluginImage {
      public:
        using Resolver = std::function<std::uint64_t(std::string_view)>;
        void Load(std::span<const std::uint8_t> nro, std::uint64_t base, const Resolver &resolver);
        std::uint64_t Symbol(std::string_view name) const;
        std::span<const std::uint8_t> Bytes() const { return image; }
        std::uint64_t Base() const { return base; }
        std::size_t TextSize() const { return textSize; }
        const std::vector<std::uint64_t> &Initializers() const { return initializers; }
        const std::vector<std::uint64_t> &Finalizers() const { return finalizers; }
      private:
        std::vector<std::uint8_t> image;
        std::map<std::string, std::uint64_t, std::less<>> symbols;
        std::vector<std::uint64_t> initializers, finalizers;
        std::uint64_t base{};
        std::size_t textSize{};
    };
}
