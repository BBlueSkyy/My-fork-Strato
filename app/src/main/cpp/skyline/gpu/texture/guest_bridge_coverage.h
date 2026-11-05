// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace skyline::gpu::texture {
    // A byte run in the concatenated guest mapping, NOT a physical address and
    // NOT proof of ownership. Translate mapping offsets to resident physical
    // spans before adding these runs to a regional-authority ledger.
    struct GuestBridgeByteRun {
        uint64_t begin{}, end{}; // half-open; never includes encoder padding
    };

    // Caller supplies the actual per-mip/layer layout from GetBlockLinearMipLayout
    // and GetBlockLinearWidthGobs. widthBytes is in native format blocks (bpb),
    // rows is in native block rows; both exclude storage padding. Supports only
    // block-linear, depth=1, guest-format-preserving 2D surfaces. Each layer/mip
    // is a separate surface at its real byte offset, including any layer gaps.
    struct GuestBridgeSurface2D {
        uint64_t offset{};
        uint64_t widthBytes{}, rows{};
        uint64_t storageGobsX{}; // includes TIC tile-width-spacing padding
        uint32_t blockHeightGobs{};
    };

    // Preflight-only coverage for the *bytes written by the native block-linear
    // encoder* (CopyBlockLinearInternal<false>). In particular, it excludes
    // trailing partial sectors, edge GOBs and the padding between levels/layers.
    // No CPU/GPU readback, synchronization, allocation or authority publication
    // is performed here. Rejects overlapping surfaces and bounded-work overflow.
    inline std::optional<std::vector<GuestBridgeByteRun>> PlanGuestBridgeByteCoverage(
        std::span<const GuestBridgeSurface2D> surfaces, uint64_t mappedBytes,
        size_t maxChunks = 262144, uint64_t maxGobs = 65536) {
        constexpr uint64_t GobWidth{64}, GobHeight{8}, GobBytes{512}, SectorBytes{16};
        constexpr auto Max{std::numeric_limits<uint64_t>::max()};
        if (surfaces.empty() || !mappedBytes || !maxChunks || !maxGobs)
            return std::nullopt;
        std::vector<GuestBridgeByteRun> chunks;
        uint64_t totalGobs{};
        for (const auto &surface : surfaces) {
            const auto bh{static_cast<uint64_t>(surface.blockHeightGobs)};
            if (!surface.widthBytes || !surface.rows || !bh || (bh & (bh - 1)) ||
                surface.offset >= mappedBytes)
                return std::nullopt;
            // Divide before rounding to avoid overflow on very large dimensions.
            const auto usefulGobsX{surface.widthBytes / GobWidth + (surface.widthBytes % GobWidth != 0)};
            const auto usefulGobsY{surface.rows / GobHeight + (surface.rows % GobHeight != 0)};
            if (surface.storageGobsX < usefulGobsX || !surface.storageGobsX ||
                usefulGobsX > maxGobs || usefulGobsY > maxGobs ||
                usefulGobsX > (maxGobs - totalGobs) / usefulGobsY)
                return std::nullopt;
            totalGobs += usefulGobsX * usefulGobsY;

            // Validate the last address before iterating. GOBs are ordered by
            // block-row, physical column, then the GOB's Y position in a block.
            const auto blockRows{(usefulGobsY - 1) / bh + 1};
            if (blockRows - 1 > Max / surface.storageGobsX)
                return std::nullopt;
            const auto columnBase{(blockRows - 1) * surface.storageGobsX};
            if (usefulGobsX - 1 > Max - columnBase) return std::nullopt;
            const auto lastColumn{columnBase + usefulGobsX - 1};
            if (lastColumn > (Max - (usefulGobsY - 1) % bh) / bh)
                return std::nullopt;
            const auto lastGob{lastColumn * bh + (usefulGobsY - 1) % bh};
            if (surface.offset > mappedBytes ||
                lastGob > (mappedBytes - surface.offset) / GobBytes ||
                mappedBytes - surface.offset - lastGob * GobBytes < GobBytes)
                return std::nullopt;

            auto add = [&](uint64_t begin, uint64_t end) {
                if (chunks.size() == maxChunks) return false;
                chunks.push_back({begin, end});
                return true;
            };
            for (uint64_t gy{}; gy < usefulGobsY; ++gy) {
                const auto validRows{std::min(GobHeight, surface.rows - gy * GobHeight)};
                for (uint64_t gx{}; gx < usefulGobsX; ++gx) {
                    const auto validBytes{std::min(GobWidth, surface.widthBytes - gx * GobWidth)};
                    const auto gob{((gy / bh) * surface.storageGobsX + gx) * bh + gy % bh};
                    const auto base{surface.offset + gob * GobBytes};
                    if (validRows == GobHeight && validBytes == GobWidth) {
                        if (!add(base, base + GobBytes)) return std::nullopt;
                        continue;
                    }
                    // Sector order matches CopyBlockLinearInternal: X/Y are
                    // Morton-swizzled, but every written sector is contiguous.
                    for (uint32_t sector{}; sector < 32; ++sector) {
                        const uint64_t x{((sector << 3) & 16) | ((sector << 1) & 32)};
                        const uint64_t y{((sector >> 1) & 6) | (sector & 1)};
                        if (x >= validBytes || y >= validRows) continue;
                        const auto begin{base + sector * SectorBytes};
                        if (!add(begin, begin + std::min(SectorBytes, validBytes - x)))
                            return std::nullopt;
                    }
                }
            }
        }
        std::sort(chunks.begin(), chunks.end(), [](auto &a, auto &b) { return a.begin < b.begin; });
        std::vector<GuestBridgeByteRun> result;
        result.reserve(chunks.size());
        for (const auto &run : chunks) {
            if (!result.empty() && run.begin < result.back().end)
                return std::nullopt; // self-alias/overlapping levels need a different policy
            if (!result.empty() && run.begin == result.back().end)
                result.back().end = run.end;
            else
                result.push_back(run);
        }
        return result;
    }

    // The host's guest-texture mappings represent a logical byte sequence, but
    // consecutive mappings may point to unrelated physical pages. Resolve only
    // proved encoder bytes, splitting runs at every mapping boundary. Never
    // treat a contiguous logical offset as a contiguous physical address.
    struct GuestBridgeMapping {
        uintptr_t physicalBegin{};
        uint64_t length{};
    };
    struct GuestBridgePhysicalRun {
        uintptr_t begin{}, end{};
        uint64_t guestOffset{};
    };

    inline std::optional<std::vector<GuestBridgePhysicalRun>> ResolveGuestBridgePhysicalRuns(
        std::span<const GuestBridgeByteRun> covered,
        std::span<const GuestBridgeMapping> mappings, size_t maxRuns = 262144) {
        constexpr auto MaxAddress{std::numeric_limits<uintptr_t>::max()};
        constexpr auto MaxOffset{std::numeric_limits<uint64_t>::max()};
        if (covered.empty() || mappings.empty() || !maxRuns || mappings.size() > 512)
            return std::nullopt;
        uint64_t total{};
        for (const auto &mapping : mappings) {
            if (!mapping.physicalBegin || !mapping.length ||
                mapping.length > MaxAddress - mapping.physicalBegin ||
                mapping.length > MaxOffset - total) return std::nullopt;
            total += mapping.length;
        }
        uint64_t previousEnd{};
        std::vector<GuestBridgePhysicalRun> physical;
        for (const auto &run : covered) {
            if (run.begin >= run.end || run.begin < previousEnd || run.end > total)
                return std::nullopt;
            previousEnd = run.end;
            uint64_t mappingOffset{}, mapped{};
            for (const auto &mapping : mappings) {
                const auto first{std::max(run.begin, mappingOffset)};
                const auto last{std::min(run.end, mappingOffset + mapping.length)};
                if (first < last) {
                    if (physical.size() == maxRuns) return std::nullopt;
                    const auto begin{mapping.physicalBegin + static_cast<uintptr_t>(first - mappingOffset)};
                    physical.push_back({begin, begin + static_cast<uintptr_t>(last - first), first});
                    mapped += last - first;
                }
                mappingOffset += mapping.length;
                if (mappingOffset >= run.end) break;
            }
            if (mapped != run.end - run.begin) return std::nullopt;
        }
        // A resource mapping the same physical allocation twice needs a
        // separate self-alias policy; reject instead of double-claiming bytes.
        std::sort(physical.begin(), physical.end(), [](const auto &a, const auto &b) {
            return a.begin < b.begin;
        });
        std::vector<GuestBridgePhysicalRun> result;
        result.reserve(physical.size());
        for (const auto &run : physical) {
            if (!result.empty() && run.begin < result.back().end) return std::nullopt;
            if (!result.empty() && run.begin == result.back().end &&
                result.back().guestOffset + (result.back().end - result.back().begin) == run.guestOffset)
                result.back().end = run.end;
            else result.push_back(run);
        }
        return result;
    }
}
