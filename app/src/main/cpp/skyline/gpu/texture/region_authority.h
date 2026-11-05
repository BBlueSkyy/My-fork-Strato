// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "alias_group.h"
#include <cstddef>
#include <algorithm>

namespace skyline::gpu { class Buffer; }

namespace skyline::gpu::texture {
    // Release gate: enable only after the combined production Buffer/manager/
    // executor Vulkan fixture proves publication, callbacks and migration.
    inline constexpr bool MixedRegionAuthorityEnabled{true};
    // Dense rows or bounded whole GOBs. Padding never enters image authority;
    // endpoints and explicit subresource coordinates are proved at registration.
    struct PhysicalImageSpan {
        uintptr_t begin{}, end{};
        size_t offset{}, rowBytes{};
        uint32_t level{}, layer{}, y{}, rowHeight{}, x{}, z{}, width{};
    };
    struct PhysicalImageRegion {
        uintptr_t begin{}, end{};
        Texture *writerIdentity{};
        std::weak_ptr<Texture> writer;
        uint64_t hostVersion{}, guestVersion{};
        Buffer *bufferWriterIdentity{};
        std::weak_ptr<Buffer> bufferWriter;
        bool HasWriter() const { return writerIdentity || bufferWriterIdentity; }
    };
    struct PhysicalBufferBinding {
        size_t region{}, offset{};
        uint64_t residentVersion{};
    };
    struct PhysicalImageBinding {
        size_t region{}, offset{};
        uint32_t level{}, layer{}, y{};
        size_t rowBytes{};
        uint32_t rowHeight{};
        uint64_t residentVersion{};
        uint32_t x{}, z{}, width{};
    };
    struct RegionAuthority {
        // Acquire after resource locks. Trap callbacks use try_lock; no waiting
        // for resources, traps or guest faults while holding this mutex.
        std::recursive_mutex mutex;
        std::vector<std::weak_ptr<Texture>> members;
        std::vector<PhysicalImageRegion> regions;
        std::vector<std::weak_ptr<Buffer>> buffers;
    };
    // Descriptor reuse checks one scalar. Increment only on a newly invalidated
    // overlapping host, never on ordinary writes to independent resources.
    inline std::atomic<uint64_t> regionInvalidationSequence{};
}
