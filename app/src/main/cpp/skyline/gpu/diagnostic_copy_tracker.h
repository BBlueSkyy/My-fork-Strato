// SPDX-License-Identifier: MPL-2.0
#pragma once

// Draft PR #332: correlate DMA guest writes with the real NvMap-backed
// graphic-buffer surfaces without storing Title IDs or changing copy behavior.
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <atomic>

namespace skyline::gpu::diagnostic {
    struct PresentRegion {
        uintptr_t base{};
        size_t size{};
        uint32_t width{}, height{};
        uint32_t nvmapId{};
        uint64_t registration{};
    };

    inline constexpr uint32_t DrawTextureProbeVersion{3};

    inline std::atomic<uint64_t> &DrawTextureTriggers() {
        static std::atomic<uint64_t> count{};
        return count;
    }

    // A mapped destination overlaps a registered allocation; this is a
    // candidate, not proof that the ignored method wrote any pixels.
    inline std::atomic<uint64_t> &DrawTexturePresentCandidates() {
        static std::atomic<uint64_t> count{};
        return count;
    }

    inline std::atomic<uint64_t> &DrawTextureUnknownExtents() {
        static std::atomic<uint64_t> count{};
        return count;
    }

    inline std::mutex &PresentRegionMutex() {
        static std::mutex mutex;
        return mutex;
    }

    inline std::array<PresentRegion, 24> &PresentRegions() {
        static std::array<PresentRegion, 24> regions{};
        return regions;
    }

    inline size_t &PresentRegionCount() {
        static size_t count{};
        return count;
    }

    inline std::atomic<size_t> &LoggedDmaMatches() {
        static std::atomic<size_t> count{};
        return count;
    }

    inline std::atomic<size_t> &LoggedBlitMatches() {
        static std::atomic<size_t> count{};
        return count;
    }

    inline std::atomic<size_t> &LoggedInlineMatches() {
        static std::atomic<size_t> count{};
        return count;
    }

    inline std::atomic<size_t> &LoggedGpuTextureWrites() {
        static std::atomic<size_t> count{};
        return count;
    }

    inline std::atomic<size_t> &LoggedGpuBufferWrites() {
        static std::atomic<size_t> count{};
        return count;
    }

    // Independent of the 160-entry unique-RT trace limit: these counters
    // cover the whole emulation session, including render passes after the
    // early startup batch has exhausted the unique-RT diagnostic slots.
    inline std::atomic<uint64_t> &RenderTargetAttachmentVisits() {
        static std::atomic<uint64_t> count{};
        return count;
    }

    inline std::atomic<uint64_t> &RenderTargetAttachmentPresentMatches() {
        static std::atomic<uint64_t> count{};
        return count;
    }

    inline std::atomic<uint64_t> &RenderTargetAttachmentLastMatch() {
        static std::atomic<uint64_t> guestAddress{};
        return guestAddress;
    }

    inline void TrackPresentationSurface(const void *base, size_t size,
                                         uint32_t width, uint32_t height,
                                         uint32_t nvmapId = 0) {
        if (!base || !size)
            return;
        const auto address{reinterpret_cast<uintptr_t>(base)};
        std::scoped_lock lock{PresentRegionMutex()};
        auto &regions{PresentRegions()};
        auto &count{PresentRegionCount()};
        static uint64_t registration{}; // protected by PresentRegionMutex
        const auto generation{++registration};
        for (size_t i{}; i < count; ++i) {
            if (regions[i].base == address) {
                regions[i] = {address, size, width, height, nvmapId, generation};
                return;
            }
        }
        if (count < regions.size())
            regions[count++] = {address, size, width, height, nvmapId, generation};
    }

    inline std::optional<PresentRegion> FindPresentationOverlap(const void *address, size_t size) {
        if (!address || !size)
            return std::nullopt;
        const auto ptr{reinterpret_cast<uintptr_t>(address)};
        std::scoped_lock lock{PresentRegionMutex()};
        std::optional<PresentRegion> result;
        for (size_t i{}; i < PresentRegionCount(); ++i) {
            const auto &region{PresentRegions()[i]};
            // Subtraction avoids overflowing either interval's end address.
            const bool overlaps{ptr >= region.base ? ptr - region.base < region.size
                                                   : region.base - ptr < size};
            // Reallocation can leave older overlapping diagnostic entries.
            // Prefer the most recently registered candidate, and log its ID.
            if (overlaps && (!result || region.registration > result->registration))
                result = region;
        }
        return result;
    }

    inline std::optional<PresentRegion> FindPresentationSurface(const void *address) {
        if (!address)
            return std::nullopt;
        const auto ptr{reinterpret_cast<uintptr_t>(address)};
        std::scoped_lock lock{PresentRegionMutex()};
        for (size_t i{}; i < PresentRegionCount(); ++i) {
            const auto &region{PresentRegions()[i]};
            if (ptr >= region.base && ptr - region.base < region.size)
                return region;
        }
        return std::nullopt;
    }
}
