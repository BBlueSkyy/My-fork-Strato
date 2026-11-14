// SPDX-License-Identifier: MPL-2.0
#pragma once

// Temporary, bounded TIC diagnostics. No GPU-visible state or caches are changed.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace skyline::gpu::interconnect::tic_probe {
    struct ShaderContext {
        const char *kind{"runtime"};
        std::uint32_t stage{};
        std::uint64_t hash{};
    };

    inline thread_local ShaderContext currentShader{};

    struct ScopedShaderContext {
        ShaderContext previous;
        ScopedShaderContext(const char *kind, std::uint32_t stage, std::uint64_t hash)
            : previous{currentShader} {
            currentShader = {kind, stage, hash};
        }
        ~ScopedShaderContext() {
            currentShader = previous;
        }
        ScopedShaderContext(const ScopedShaderContext &) = delete;
        ScopedShaderContext &operator=(const ScopedShaderContext &) = delete;
    };

    template<typename T>
    std::array<std::uint32_t, 8> SnapshotWords(const T &descriptor) {
        static_assert(sizeof(T) == 32 && std::is_trivially_copyable_v<T>);
        std::array<std::uint32_t, 8> words{};
        // Copy the 32 descriptor bytes already read by the existing TIC path.
        // Never dereference the guest image IOVA to obtain the descriptor.
        std::memcpy(words.data(), &descriptor, sizeof(descriptor));
        return words;
    }

    struct ReservedSlot {
        std::uint64_t poolBase{};
        std::uint32_t index{};
        bool active{};
    };

    inline thread_local std::array<ReservedSlot, 32> reservedSlots{};
    inline thread_local std::uint32_t nextReservedSlot{};
    inline thread_local bool anyReservedSlots{};

    inline void RememberReserved(std::uint64_t poolBase, std::uint32_t index) {
        for (auto &entry : reservedSlots)
            if (entry.active && entry.poolBase == poolBase && entry.index == index)
                return;
        reservedSlots[nextReservedSlot++ % reservedSlots.size()] = {poolBase, index, true};
        anyReservedSlots = true;
    }

    inline bool ConsumeRecovered(std::uint64_t poolBase, std::uint32_t index) {
        if (!anyReservedSlots)
            return false;
        for (auto &entry : reservedSlots) {
            if (entry.active && entry.poolBase == poolBase && entry.index == index) {
                entry.active = false;
                return true;
            }
        }
        return false;
    }

    // Deduplicate equivalent events on each thread and cap log volume globally.
    // The cap applies only to the rare invalid-TIC path, never to normal draws.
    inline std::atomic<std::uint32_t> emittedEvents{0};
    inline thread_local std::array<std::uint64_t, 64> recentFingerprints{};
    inline thread_local std::uint32_t nextFingerprint{};
    inline constexpr std::uint32_t MaxEmittedEvents{96};

    inline std::uint64_t Fingerprint(std::uint64_t poolBase, std::uint32_t index,
                                     const std::array<std::uint32_t, 8> &words,
                                     std::uint64_t shaderHash, std::uint32_t site) {
        std::uint64_t hash{14695981039346656037ULL};
        auto add{[&](std::uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }};
        add(poolBase);
        add(index);
        for (auto word : words)
            add(word);
        add(shaderHash);
        add(site);
        return hash ? hash : 1;
    }

    inline bool ShouldEmit(std::uint64_t fingerprint) {
        if (!fingerprint)
            fingerprint = 1;
        for (auto seen : recentFingerprints)
            if (seen == fingerprint)
                return false;

        recentFingerprints[nextFingerprint++ % recentFingerprints.size()] = fingerprint;
        return emittedEvents.fetch_add(1, std::memory_order_relaxed) < MaxEmittedEvents;
    }
}
