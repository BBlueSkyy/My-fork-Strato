// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "alias_group.h"
#include <cstddef>
#include <algorithm>
#include <vulkan/vulkan.hpp> // MixedRawFormatSupported uses vk::Format in this header.

namespace skyline::gpu { class Buffer; }

namespace skyline::gpu::texture {
    // Release gate: enable only after the combined production Buffer/manager/
    // executor Vulkan fixture proves publication, callbacks and migration.
    inline constexpr bool MixedRegionAuthorityEnabled{false};
    // Experimental (3e1e0892, e4aaaf70): when registration rejects a layout and no
    // participant is tracked yet, silently continue on the legacy image-only path.
    // That path installs NO persistent relation, so a later buffer GPU write does
    // not invalidate the overlapping image (and vice versa): stale reads. It is a
    // known coherence hole (docs/region-stage-c-fallback.md), therefore opt-in only
    // and OFF by default. With it off, unsupported overlap rejects explicitly.
    inline constexpr bool MixedUnsupportedLegacyFallback{false};
    // Staged reversible transport in development; requires complete CPU/GPU
    // coherence, trap and Vulkan validation before release activation.
    inline constexpr bool MixedGuestBridgeExperimental{false};
    // Formats the raw GOB buffer<->image transport (PlanRawRgba8GobCopies) can copy
    // byte-exactly: 4 bytes/texel, 1x1 blocks, packed in 16-byte sectors, mip 0 /
    // layer 0. sRGB shares bytes and GOB geometry with UNORM; Vulkan copies never
    // convert, and UNORM/sRGB are size-compatible for vkCmdCopyImage. Any other
    // format needs a generalized planner (bytes/block, block extent, subresource)
    // and barriers over every level/layer before it may be admitted here.
    constexpr bool MixedRawFormatSupported(vk::Format format) {
        return format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
    }
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
    enum class RegionTransport : uint8_t { RawGobs, GuestBridge };
    struct RegionAuthority {
        RegionTransport transport{RegionTransport::RawGobs};
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
