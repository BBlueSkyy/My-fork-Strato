// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "alias_group.h"
#include <cstddef>
#include <algorithm>

namespace skyline::gpu::texture {
    // Only dense, whole block-linear rows enter this domain. No per-texel
    // footprint bitmap: endpoints are established once at registration.
    struct PhysicalImageSpan {
        uintptr_t begin{}, end{};
        size_t offset{}, rowBytes{};
        uint32_t level{}, layer{}, y{}, rowHeight{};
    };
    struct PhysicalImageRegion {
        uintptr_t begin{}, end{};
        Texture *writerIdentity{};
        std::weak_ptr<Texture> writer;
        uint64_t hostVersion{}, guestVersion{};
    };
    struct PhysicalImageBinding {
        size_t region{}, offset{};
        uint32_t level{}, layer{}, y{};
        size_t rowBytes{};
        uint32_t rowHeight{};
        uint64_t residentVersion{};
    };
    struct RegionAuthority {
        // Acquire after resource locks. Trap callbacks use try_lock; no waiting
        // for resources, traps or guest faults while holding this mutex.
        std::recursive_mutex mutex;
        std::vector<std::weak_ptr<Texture>> members;
        std::vector<PhysicalImageRegion> regions;
    };
    // Descriptor reuse checks one scalar. Increment only on a newly invalidated
    // overlapping host, never on ordinary writes to independent resources.
    inline std::atomic<uint64_t> regionInvalidationSequence{};
}
