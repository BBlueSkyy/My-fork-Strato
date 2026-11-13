// SPDX-License-Identifier: MPL-2.0
// Temporary bounded observation of descriptors already selected by the renderer.
#pragma once
#include <array>
#include <bit>
#include <cstdint>

namespace skyline::gpu::diagnostic {
    inline constexpr uint64_t HdrReductionShader{0x42016CE58CA1A259ULL};
    inline constexpr uint32_t HdrDrawLimit{12};
    inline constexpr uint32_t HdrBindingLimit{8};
    struct HdrSampleBinding {
        bool valid{}, ticKnown{};
        uint32_t handle{}, effectiveTscIndex{}, stage{}, cbuf{}, offset{}, element{};
        uint64_t sampler{}, imageView{}, image{}, generation{}, sequence{};
        uintptr_t texture{};
        uint32_t baseMip{}, mipCount{}, baseLayer{}, layerCount{};
        std::array<uint32_t, 8> tic{};
    };
    constexpr void HdrMix(uint64_t &hash, uint64_t value) {
        hash = (hash ^ value) * 1099511628211ULL;
    }
    inline bool HdrAdmit(std::array<uint64_t, HdrDrawLimit> &signatures, uint32_t &count, uint64_t signature) {
        for (uint32_t i{}; i < count; ++i) if (signatures[i] == signature) return false;
        if (count == HdrDrawLimit) return false;
        signatures[count++] = signature;
        return true;
    }
    constexpr bool HdrDrawSelected(uint64_t shader, uint32_t width, uint32_t height, uint32_t count) {
        return shader == HdrReductionShader && width == 320 && height == 180 && count < HdrDrawLimit;
    }
}
