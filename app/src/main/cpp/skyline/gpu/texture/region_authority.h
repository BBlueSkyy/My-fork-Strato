// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "alias_group.h"
#include <cstddef>
#include <algorithm>

namespace skyline::gpu { class Buffer; }

namespace skyline::gpu::texture {
    // Release gate: enable only after the combined production Buffer/manager/
    // executor Vulkan fixture proves publication, callbacks and migration.
    inline constexpr bool MixedRegionAuthorityEnabled{false};
    inline constexpr size_t GuestBridgeMaxRegions{4096};
    inline constexpr size_t GuestBridgeMaxEndpoints{GuestBridgeMaxRegions * 2};
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
        const bool dependencyOnly;
        explicit RegionAuthority(bool dependencies = false) : dependencyOnly(dependencies) {}
        // Acquire after resource locks. Trap callbacks use try_lock; no waiting
        // for resources, traps or guest faults while holding this mutex.
        std::recursive_mutex mutex;
        // Guest transport is independent of mixed membership and raw GOB copies.
        bool guestBridge{};
        std::vector<std::weak_ptr<Texture>> members;
        std::vector<PhysicalImageRegion> regions;
        std::vector<std::weak_ptr<Buffer>> buffers;
    };
    // A new identity and lifetime after withdrawal; never owns the old ledger.
    // Reuse the byte partition/transport machinery, not the active membership.
    struct GuestDependencyDomain final : RegionAuthority {
        GuestDependencyDomain() : RegionAuthority(true) { guestBridge = true; }
    };
    class RegionLink {
        AtomicSharedPtr<RegionAuthority> tracking;
      public:
        // Active authority and withdrawn dependencies share the existing
        // preparation/publication protocol. Stable lookup is one atomic load.
        std::shared_ptr<RegionAuthority> load() const noexcept { return tracking.load(); }
        std::shared_ptr<RegionAuthority> active() const noexcept {
            auto value{load()}; return value && !value->dependencyOnly ? value : nullptr;
        }
        std::shared_ptr<RegionAuthority> dependency() const noexcept {
            auto value{load()}; return value && value->dependencyOnly ? value : nullptr;
        }
        // All resources/states and previous-domain locks protect the short
        // replacement. The old object is released only after installing next.
        void store(std::shared_ptr<RegionAuthority> value) noexcept { tracking.store(std::move(value)); }
    };
    // Descriptor reuse checks one scalar. Increment only on a newly invalidated
    // overlapping host, never on ordinary writes to independent resources.
    inline std::atomic<uint64_t> regionInvalidationSequence{};
}
