// SPDX-License-Identifier: MPL-2.0
#pragma once

// PR453 temporary device-only, read-only diagnostic. Not a renderer fix.
// The host Vulkan fixture extracts production methods without this header.
#if defined(__ANDROID__)
#include <atomic>
#include <logger/logger.h>
#include "texture.h"

namespace skyline::gpu::mip_history_probe {
    // The observed MK8 overlap is a 640x360 R11G11B10 HDR, one/two-mip
    // texture. No hard-coded guest addresses (ASLR/mappings vary by boot).
    inline bool Relevant(const GuestTexture &guest) {
        return guest.format && guest.format->vkFormat == vk::Format::eB10G11R11UfloatPack32 &&
            guest.dimensions.width == 640 && guest.dimensions.height == 360 &&
            guest.dimensions.depth == 1 && (guest.mipLevelCount == 1 || guest.mipLevelCount == 2);
    }

    // Shared across translation units. Drop logs rather than overflow logger.
    inline std::atomic<u32> emitted{};
    inline bool Once(std::atomic<u32> &flags, u32 bit) {
        if (flags.fetch_or(bit, std::memory_order_relaxed) & bit) return false;
        return emitted.fetch_add(1, std::memory_order_relaxed) < 96;
    }
    constexpr u32 PrefixAccepted = 1u << 0;
    constexpr u32 PrefixRejected = 1u << 1;
    constexpr u32 DonorAccepted = 1u << 2;
    constexpr u32 DonorRejected = 1u << 3;
    constexpr u32 ConsolidationBlocked = 1u << 4;
    constexpr u32 PromotionDone = 1u << 5;
    constexpr u32 NewParent = 1u << 6;
    constexpr u32 SampleOriginal = 1u << 7;
    constexpr u32 SamplePromoted = 1u << 8;
    constexpr u32 RenderTargetOriginal = 1u << 9;
    constexpr u32 RenderTargetPromoted = 1u << 10;
    constexpr u32 ViewRecreated = 1u << 11;
    constexpr u32 FallbackToSeparate = 1u << 12;
}
#endif
