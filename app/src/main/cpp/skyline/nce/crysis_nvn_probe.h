// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

// Temporary PR #410 diagnostics. These are offsets in the verified update SDK,
// not an NVN HLE implementation. Remove the probes when the cause is established.
namespace skyline::nce::crysis {
    struct Probe {
        std::uint16_t svc;
        std::size_t offset;
        std::array<std::uint32_t, 4> signature;
        std::string_view name;
    };

    inline constexpr std::array<Probe, 4> SdkProbes{{
        {0x7FF1, 0x45AD80, {0xF81B0FFA, 0xA90163F9, 0xA9025BF7, 0xA90353F5}, "AddCommandMemory"},
        {0x7FF2, 0x45AF40, {0xA9BE53F5, 0xA9017BF3, 0xF9402C08, 0xAA0203F5}, "AddControlMemory"},
        {0x7FF3, 0x45B080, {0xA9BF7BF3, 0xF9400C08, 0xF9001408, 0x320003EA}, "BeginRecording"},
        {0x7FF4, 0x45B120, {0xD10103FF, 0xA9015BF7, 0xA90253F5, 0xA9037BF3}, "EndRecording"},
    }};

    constexpr std::uint32_t ReplayInstruction(std::uint16_t svc) {
        if (svc == 0x7FF0)
            return 0xF81F0FFE; // Existing main callback: STR LR, [SP, #-16]!
        for (const auto &probe : SdkProbes)
            if (svc == probe.svc)
                return probe.signature[0]; // All are position-independent prologues.
        return 0;
    }

    inline bool InstallSdkProbes(std::vector<std::uint8_t> &text, std::uint64_t title,
                                 std::string_view name, std::span<const std::uint8_t> buildId) {
        constexpr std::array<std::uint8_t, 32> ExpectedBuild{
            0x2F,0x08,0x86,0x2A,0x30,0x91,0x1F,0x87,0xA1,0x3B,0x11,0xF7,0xDD,0x0F,0x50,0x9D,
            0xCB,0x42,0x2D,0x2D};
        if (title != 0x0100E66010ADE000ULL || name != "sdk.nso" ||
            buildId.size() != ExpectedBuild.size() ||
            std::memcmp(buildId.data(), ExpectedBuild.data(), ExpectedBuild.size()))
            return false;
        // Validate the entire set before changing any executable bytes.
        for (const auto &probe : SdkProbes)
            if (probe.offset > text.size() || sizeof(probe.signature) > text.size() - probe.offset ||
                std::memcmp(text.data() + probe.offset, probe.signature.data(), sizeof(probe.signature)))
                return false;
        for (const auto &probe : SdkProbes) {
            const std::uint32_t instruction{0xD4000001U | (static_cast<std::uint32_t>(probe.svc) << 5)};
            std::memcpy(text.data() + probe.offset, &instruction, sizeof(instruction));
        }
        return true;
    }

    struct Usage {
        bool valid{};
        std::uint64_t size{}, used{}, free{};
    };

    inline Usage GetUsage(std::uint64_t start, std::uint64_t cursor, std::uint64_t end) {
        if (start > cursor || cursor > end)
            return {};
        return {true, end - start, cursor - start, end - cursor};
    }

    struct State {
        Usage command, control;
    };

    inline State Decode(const std::array<std::uint64_t, 15> &raw) {
        // Equivalent to the six getters at SDK+45B010..45B07C. Do not call
        // guest code from the host or derive a cursor from volatile x4.
        State result{GetUsage(raw[0x50 / 8], raw[0x18 / 8], raw[0x20 / 8]), {}};
        if (raw[0x38 / 8] <= std::numeric_limits<std::uint64_t>::max() - 16)
            result.control = GetUsage(raw[0x58 / 8], raw[0x30 / 8], raw[0x38 / 8] + 16);
        return result;
    }
}
