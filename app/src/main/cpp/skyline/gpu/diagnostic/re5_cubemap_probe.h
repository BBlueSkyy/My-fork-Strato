// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace skyline::gpu::diagnostic {
    // View counts are relative to the actual VkImage allocation, not guest TIC
    // mip metadata. VK_REMAINING_* is represented by UINT32_MAX.
    constexpr bool DiagnosticSubresourceFits(std::uint32_t allocated,
                                              std::uint32_t base, std::uint32_t count) {
        return base < allocated &&
               (count == UINT32_MAX || (count != 0 && count <= allocated - base));
    }

    // Diagnostic selection only. This must never influence game rendering or
    // Texman reuse decisions.
    constexpr bool DiagnosticCubeCandidate(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t depth, std::uint32_t layers,
                                            std::uint32_t mips, std::size_t bytes,
                                            bool rgba8, bool transferable, bool generalLayout) {
        return width == 128 && height == 128 && depth == 1 &&
               layers == 6 && mips >= 1 && mips <= 8 &&
               bytes > 0 && bytes <= 2ULL * 1024 * 1024 &&
               rgba8 && transferable && generalLayout;
    }
}
