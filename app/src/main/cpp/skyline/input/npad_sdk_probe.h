// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace skyline::input::diagnostic {
    using std::size_t;
    // Temporary diagnostic: only these exact, demangled SDK ABIs are decoded.
    // Unknown signatures are reported, never guessed from register contents.
    struct NpadReader {
        size_t layout;
        bool plural;
    };

    inline constexpr std::array<std::string_view, 7> Layouts{
        "NpadFullKeyState", "NpadHandheldState", "NpadJoyDualState",
        "NpadJoyLeftState", "NpadJoyRightState", "system::NpadSystemState",
        "system::NpadSystemExtState",
    };

    inline std::optional<NpadReader> IdentifyReader(std::string_view name) {
        for (size_t layout{}; layout < Layouts.size(); ++layout) {
            for (bool plural : {false, true}) {
                const auto prefix{std::string{"nn::hid::GetNpadState"} + (plural ? "s" : "")};
                const auto type{std::string{"nn::hid::"} + std::string{Layouts[layout]}};
                const auto parameters{std::string{"("} + type + "*, " +
                    (plural ? "int, " : "") + "unsigned int const&)"};
                if (name == prefix + parameters ||
                    name == "nn::hid::system::GetNpadState" + std::string{plural ? "s" : ""} + parameters)
                    return NpadReader{layout, plural};
            }
        }
        return std::nullopt;
    }

    struct NpadOutput {
        std::uint64_t samplingNumber;
        std::uint64_t buttons;
        std::int32_t lx, ly, rx, ry;
        std::uint32_t attributes, reserved;
        bool operator==(const NpadOutput &) const = default;
    };
    static_assert(sizeof(NpadOutput) == 0x28);

    inline std::optional<size_t> NpadSlot(std::uint32_t id) {
        if (id < 8)
            return id;
        if (id == 0x20)
            return 8;
        if (id == 0x10)
            return 9;
        return std::nullopt;
    }
}
