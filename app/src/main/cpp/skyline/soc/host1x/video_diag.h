// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#pragma once

#include <common.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace skyline::soc::host1x::video_diag {
    // Diagnostic-only: bounded, sampled work; never changes NVDEC/VIC/texture state.
    inline bool Sample(u64 sequence) {
        return sequence <= 24 || sequence % 120 == 0;
    }

    struct PixelProbe {
        u32 samples{};
        u32 aboveDark{}; // Sample values greater than 32 (not a proof of visibility).
        u8 minimum{255};
        u8 maximum{};
        u64 hash{1469598103934665603ULL};
    };

    inline void AddSample(PixelProbe &probe, u8 value) {
        ++probe.samples;
        probe.aboveDark += value > 32;
        probe.minimum = std::min(probe.minimum, value);
        probe.maximum = std::max(probe.maximum, value);
        probe.hash = (probe.hash ^ value) * 1099511628211ULL;
    }

    // Exactly 64 spatial samples: valid for luminance and packed byte surfaces.
    inline PixelProbe ProbePlane(const u8 *pixels, size_t stride, size_t widthBytes, size_t height) {
        PixelProbe probe{};
        if (!pixels || !stride || !widthBytes || !height)
            return probe;
        for (size_t y{}; y < 8; ++y) {
            const u8 *row{pixels + (y * (height - 1) / 7) * stride};
            for (size_t x{}; x < 8; ++x)
                AddSample(probe, row[x * (widthBytes - 1) / 7]);
        }
        return probe;
    }

    // A small ring of physical CPU backing intervals associated with sampled
    // VIC outputs. GPU guest mappings use the same translated CPU pointers.
    // This is a correlation hint, not a proof of memory or image ownership.
    struct Backing {
        uintptr_t start{};
        uintptr_t end{};
        u64 iova{};
        u64 serial{};
    };

    struct BackingMatch {
        u64 iova{};
        u64 serial{};
    };

    inline std::mutex backingMutex;
    inline std::array<Backing, 32> recentBackings{};
    inline size_t backingNext{};
    inline std::atomic<bool> hasBackings{false};
    inline std::atomic<u64> backingSerial{};
    inline std::atomic<u64> textureEvents{};

    template <typename Ranges>
    inline void RememberVicOutput(const Ranges &ranges, u64 iova) {
        std::lock_guard lock(backingMutex);
        const u64 serial{++backingSerial};
        for (const auto &mapping : ranges) {
            if (!mapping.data() || mapping.empty())
                continue;
            uintptr_t start{reinterpret_cast<uintptr_t>(mapping.data())};
            recentBackings[backingNext++ % recentBackings.size()] = {
                start, start + mapping.size(), iova, serial,
            };
            hasBackings.store(true, std::memory_order_release);
        }
    }

    template <typename Ranges>
    inline std::optional<BackingMatch> MatchVicOutput(const Ranges &ranges) {
        if (!hasBackings.load(std::memory_order_acquire))
            return std::nullopt;
        std::lock_guard lock(backingMutex);
        std::optional<BackingMatch> best{};
        for (const auto &mapping : ranges) {
            if (!mapping.data() || mapping.empty())
                continue;
            uintptr_t start{reinterpret_cast<uintptr_t>(mapping.data())};
            uintptr_t end{start + mapping.size()};
            for (const auto &candidate : recentBackings) {
                if (candidate.serial && start < candidate.end && candidate.start < end &&
                    (!best || candidate.serial > best->serial))
                    best = BackingMatch{candidate.iova, candidate.serial};
            }
        }
        return best;
    }

    inline bool SampleTextureEvent() {
        return Sample(++textureEvents);
    }
}
