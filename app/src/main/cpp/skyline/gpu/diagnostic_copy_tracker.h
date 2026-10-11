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
    };

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
                                         uint32_t width, uint32_t height) {
        if (!base || !size)
            return;
        const auto address{reinterpret_cast<uintptr_t>(base)};
        std::scoped_lock lock{PresentRegionMutex()};
        auto &regions{PresentRegions()};
        auto &count{PresentRegionCount()};
        for (size_t i{}; i < count; ++i) {
            if (regions[i].base == address) {
                regions[i] = {address, size, width, height};
                return;
            }
        }
        if (count < regions.size())
            regions[count++] = {address, size, width, height};
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
