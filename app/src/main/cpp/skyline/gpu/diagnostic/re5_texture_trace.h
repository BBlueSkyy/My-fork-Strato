// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <gpu/texture/texture.h>
#include <gpu/diagnostic/re5_cubemap_probe.h>
#include "re5_blit_trace.h"

namespace skyline::gpu::diagnostic {
    inline std::vector<MemoryRange> TextureMappings(const GuestTexture &guest) {
        std::vector<MemoryRange> ranges;
        for (const auto &mapping : guest.mappings) {
            if (ranges.size() == BlitJournal::MaxRanges) break;
            ranges.push_back({reinterpret_cast<uintptr_t>(mapping.data()), mapping.size()});
        }
        return ranges;
    }
    inline std::string DescribeGuestTexture(const GuestTexture &guest) {
        auto text{fmt::format("guest_format={} dimensions={}x{}x{} view_type={} levels={} view_mip={}+{} layers={} view_layer={}+{} layer_stride_declared={} tile_mode={}",
            guest.format ? vk::to_string(guest.format->vkFormat) : "null", guest.dimensions.width,
            guest.dimensions.height, guest.dimensions.depth, vk::to_string(guest.viewType),
            guest.mipLevelCount, guest.viewMipBase, guest.viewMipCount, guest.layerCount,
            guest.baseArrayLayer, guest.GetViewLayerCount(), guest.layerStride, static_cast<u32>(guest.tileConfig.mode))};
        if (guest.tileConfig.mode == texture::TileMode::Block)
            text += fmt::format(" block_height={} block_depth={}", guest.tileConfig.blockHeight, guest.tileConfig.blockDepth);
        else if (guest.tileConfig.mode == texture::TileMode::Pitch)
            text += fmt::format(" pitch={}", guest.tileConfig.pitch);
        text += fmt::format(" mapping_count={} mapping_coverage={} mappings=", guest.mappings.size(),
            guest.mappings.size() <= BlitJournal::MaxRanges ? "complete" : "truncated");
        size_t logical{};
        size_t count{};
        for (const auto &mapping : guest.mappings) {
            if (count++ == BlitJournal::MaxRanges) break;
            text += fmt::format("[logical={},host=0x{:X},bytes={}],", logical,
                reinterpret_cast<uintptr_t>(mapping.data()), mapping.size());
            logical += mapping.size();
        }
        return text;
    }
    // Guest metadata is immutable; safe to describe a candidate without acquiring
    // its backing lock. Mutable image/layout/sync state is captured only after selection.
    inline std::string DescribeCandidate(const Texture &texture, bool compact = false) {
        std::string text{fmt::format("texture_id={} object=0x{:X}", texture.diagnosticIdentity,
            reinterpret_cast<uintptr_t>(&texture))};
        if (texture.guest) {
            if (!compact) text += " " + DescribeGuestTexture(*texture.guest);
            else {
                const auto &guest{*texture.guest};
                text += fmt::format(" guest_format={} dimensions={}x{}x{} levels={} layers={} layer_stride_declared={} mappings=",
                    vk::to_string(guest.format->vkFormat), guest.dimensions.width, guest.dimensions.height,
                    guest.dimensions.depth, guest.mipLevelCount, guest.layerCount, guest.layerStride);
                size_t count{};
                for (const auto &mapping : guest.mappings) {
                    if (count++ == 2) { text += "truncated"; break; }
                    text += fmt::format("[host=0x{:X},bytes={}],", reinterpret_cast<uintptr_t>(mapping.data()), mapping.size());
                }
            }
        }
        size_t offset{};
        size_t count{};
        text += " mip_byte_offsets=";
        for (const auto &mip : texture.mipLayouts) {
            if (count++ == 16) { text += "truncated"; break; }
            text += fmt::format("[offset={},bytes={},dimensions={}x{}x{},gob_h={},gob_d={}],",
                offset, mip.blockLinearSize, mip.dimensions.width, mip.dimensions.height,
                mip.dimensions.depth, mip.blockHeight, mip.blockDepth);
            offset += mip.blockLinearSize;
        }
        return text;
    }
    inline std::string DescribeTextureView(TextureView *view) {
        if (!view || !view->texture) return "view_status=null";
        auto &texture{*view->texture};
        const auto &range{view->range};
        return fmt::format("view_object=0x{:X} type={} format={} aspect={} mip={}+{} layer={}+{} swizzle={},{},{},{} backing_image=0x{:X} host_format={} host_dimensions={}x{}x{} layout={} flags={} usage={} {} {}",
            reinterpret_cast<uintptr_t>(view), vk::to_string(view->type),
            view->format ? vk::to_string(view->format->vkFormat) : "native", vk::to_string(range.aspectMask),
            range.baseMipLevel, range.levelCount, range.baseArrayLayer, range.layerCount,
            vk::to_string(view->mapping.r), vk::to_string(view->mapping.g),
            vk::to_string(view->mapping.b), vk::to_string(view->mapping.a),
            reinterpret_cast<uintptr_t>(static_cast<VkImage>(texture.GetBacking())),
            vk::to_string(texture.format->vkFormat), texture.dimensions.width, texture.dimensions.height,
            texture.dimensions.depth, vk::to_string(texture.layout), vk::to_string(texture.flags),
            vk::to_string(texture.usage), texture.DiagnosticStateSummary(), DescribeCandidate(texture)) +
            fmt::format(" host_allocated_mips={} host_allocated_layers={} view_mips_fit={} view_layers_fit={} view_backing_matches_current={} note=range_uses_actual_Texture_allocation_not_guest_metadata",
                texture.levelCount, texture.layerCount,
                DiagnosticSubresourceFits(texture.levelCount, range.baseMipLevel, range.levelCount),
                DiagnosticSubresourceFits(texture.layerCount, range.baseArrayLayer, range.layerCount),
                view->DiagnosticViewBacking() == texture.GetBacking());
    }
}
