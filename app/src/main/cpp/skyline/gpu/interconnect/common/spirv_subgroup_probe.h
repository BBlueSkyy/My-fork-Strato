// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace skyline::gpu::diagnostics {
    /**
     * @brief A/B-only SPIR-V rewrite: replace exactly two native subgroup shuffles
     *        with OpCopyObject of the local input value.
     *
     * This intentionally changes shader semantics. It is useful ONLY to learn
     * whether Qualcomm can create the compute pipeline without either shuffle.
     * The caller must guard it to the observed compute hash and Debug build.
     * Returns nullopt for unexpected/invalid module layout or an unexpected
     * number of shuffle ops/capabilities, leaving the shader unchanged.
     */
    inline std::optional<std::vector<std::uint32_t>>
    ReplaceNativeShufflesWithLocalCopies(std::span<const std::uint32_t> words) {
        constexpr std::uint32_t Magic{0x07230203};
        constexpr std::uint32_t OpCapability{17};
        constexpr std::uint32_t OpCopyObject{83};
        constexpr std::uint32_t OpGroupNonUniformShuffle{345};
        constexpr std::uint32_t GroupNonUniformShuffleCapability{65};
        if (words.size() < 5 || words.front() != Magic)
            return std::nullopt;

        std::vector<std::uint32_t> output{words.begin(), words.begin() + 5};
        std::size_t replaced{};
        std::size_t removedCapability{};
        for (std::size_t pos{5}; pos < words.size();) {
            const std::uint32_t instruction{words[pos]};
            const std::uint32_t count{instruction >> 16};
            const std::uint32_t opcode{instruction & 0xFFFF};
            if (count == 0 || count > words.size() - pos)
                return std::nullopt;

            if (opcode == OpCapability && count == 2 &&
                words[pos + 1] == GroupNonUniformShuffleCapability) {
                ++removedCapability;
            } else if (opcode == OpGroupNonUniformShuffle) {
                if (count != 6)
                    return std::nullopt;
                output.insert(output.end(), {
                    (4u << 16) | OpCopyObject,
                    words[pos + 1], // Result Type
                    words[pos + 2], // Result ID
                    words[pos + 4], // Original input Value (not Invocation ID)
                });
                ++replaced;
            } else {
                output.insert(output.end(), words.begin() + pos, words.begin() + pos + count);
            }
            pos += count;
        }
        if (replaced != 2 || removedCapability != 1)
            return std::nullopt;
        return output;
    }
}
