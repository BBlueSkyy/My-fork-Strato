// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "alias_group.h"
#include <algorithm>
#include <cstddef>
#include <map>

namespace skyline::gpu::texture {
    // Guest CPU-address ranges, plus their offset in a contiguous mirror.
    struct GuestByteRange {
        uintptr_t begin{}, end{};
        size_t offset{};
    };

    // Only allocated for intersecting mappings. GPU preparation/registration is
    // serialized by the existing channelLock. CPU publication additionally takes
    // operationMutex; trap callbacks must try-lock it, never wait under trapMutex.
    struct OverlapGroup {
        std::recursive_mutex operationMutex;
        std::vector<std::weak_ptr<Texture>> members;
        std::vector<uint8_t> encoding; // One reusable scratch per component, not per cached image.
        struct Region {
            uintptr_t end;
            Texture *identity;
            std::weak_ptr<Texture> owner;
            uint64_t serial;
            bool guestValid;
        };
        std::map<uintptr_t, Region> regions;

        void Split(uintptr_t address) {
            auto next{regions.upper_bound(address)};
            if (next == regions.begin()) return;
            auto previous{std::prev(next)};
            if (previous->first == address || previous->second.end <= address) return;
            auto right{previous->second};
            regions.emplace(address, right); // Allocate before changing the existing interval.
            previous->second.end = address;
        }

        void Prepare(const GuestByteRange &range) {
            Split(range.begin);
            Split(range.end);
            auto it{regions.lower_bound(range.begin)};
            auto cursor{range.begin};
            while (cursor < range.end) {
                if (it == regions.end() || it->first > cursor) {
                    const auto end{it == regions.end() ? range.end : std::min(range.end, it->first)};
                    // Unclaimed bytes were already guest-authoritative. If a
                    // later allocation fails, these structural splits are safe.
                    regions.emplace_hint(it, cursor, Region{end, nullptr, {}, 0, true});
                    cursor = end;
                } else { cursor = it->second.end; ++it; }
            }
        }

        void Claim(const GuestByteRange &range, const std::shared_ptr<Texture> &owner,
                   uint64_t serial, bool valid, bool history = false) {
            Prepare(range);
            for (auto it{regions.lower_bound(range.begin)}; it != regions.end() && it->first < range.end; ++it)
                if (!history || it->second.serial <= serial)
                    it->second = Region{it->second.end, owner.get(), owner, serial, valid};
        }

        template<typename Function>
        void ForEach(const GuestByteRange &range, Function &&function) const {
            auto it{regions.upper_bound(range.begin)};
            if (it != regions.begin()) --it;
            for (; it != regions.end() && it->first < range.end; ++it) {
                const auto begin{std::max(range.begin, it->first)}, end{std::min(range.end, it->second.end)};
                if (begin < end) function(begin, end, it->second);
            }
        }

        void Published(Texture *owner) {
            for (auto &[begin, region] : regions)
                if (region.identity == owner) region.guestValid = true;
        }
    };

    struct OverlapState {
        AtomicSharedPtr<OverlapGroup> group;
        std::vector<GuestByteRange> ranges; // Actual texel bytes only; never padding.
        size_t encodingSize{};
    };

    inline bool RangesIntersect(const std::vector<GuestByteRange> &a, const std::vector<GuestByteRange> &b) {
        size_t i{}, j{};
        while (i < a.size() && j < b.size()) {
            if (a[i].begin < b[j].end && b[j].begin < a[i].end) return true;
            if (a[i].end <= b[j].end) ++i; else ++j;
        }
        return false;
    }
}
