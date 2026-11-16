// SPDX-License-Identifier: MPL-2.0
// Temporary opt-in memory diagnostics. No allocation or collection policy changes.
#pragma once
#ifdef STRATO_GPU_MEMORY_DIAGNOSTICS
#include <atomic>
#include <cstdint>

namespace skyline::gpu::diagnostics {
    struct Counters {
        std::atomic<std::uint64_t> texturesLive{};
        std::atomic<std::uint64_t> texturesCreated{};
        std::atomic<std::uint64_t> texturesDestroyed{};
        // TextureManager mapping entries retain strong ownership of Texture.
        std::atomic<std::uint64_t> mappingEntries{};
        std::atomic<std::uint64_t> mappingGuestBytes{}; // Overlapping mappings are counted repeatedly.
        std::atomic<std::uint64_t> replacedMarkEvents{}; // May count the same texture more than once.
        std::atomic<std::uint64_t> stagingLive{};
        std::atomic<std::uint64_t> stagingBytes{};
        std::atomic<std::uint64_t> stagingCreated{};
        std::atomic<std::uint64_t> megabufferChunksLive{};
        std::atomic<std::uint64_t> megabufferChunksCreated{};
    };

    inline Counters counters{};
}
#endif
