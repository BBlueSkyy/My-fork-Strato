// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 Strato Team and Contributors (https://github.com/strato-emu/)

#include <soc.h>
#include <gpu/texture/layout.h>
#include "surface_writer.h"

namespace skyline::soc::host1x {
    size_t GetSurfacePlaneSize(const SurfacePlane &plane) {
        if (!plane.widthBytes || !plane.height || plane.pitch < plane.widthBytes || plane.blockHeightLog2 > 5)
            throw std::invalid_argument("Invalid video surface dimensions or pitch");

        // Block-linear row spacing is the programmed pitch, not the visible width.
        // Pitch must describe whole GOBs; it is not a substitute for tile format.
        if (plane.blockLinear && (plane.pitch & 63))
            throw std::invalid_argument("Block-linear video pitch is not GOB aligned");

        size_t size{plane.blockLinear ?
            gpu::texture::GetBlockLinearLayerSize({plane.pitch, plane.height, 1}, 1, 1, 1, 1UL << plane.blockHeightLog2, 1) :
            static_cast<size_t>(plane.pitch) * plane.height};
        if (size > (1UL << 28) || !plane.address || plane.address >= (1ULL << 32) || size > (1ULL << 32) - plane.address)
            throw std::invalid_argument("Video surface exceeds SMMU address range");
        return size;
    }

    void ValidateSurfacePlane(const DeviceState &state, const SurfacePlane &plane) {
        size_t size{GetSurfacePlaneSize(plane)};
        auto ranges{state.soc->smmu.TranslateRange(static_cast<u32>(plane.address), static_cast<u32>(size))};
        for (auto range : ranges)
            if (!range.data())
                throw std::invalid_argument("Video surface contains unmapped SMMU memory");
    }

    void WriteSurfacePlane(const DeviceState &state, const SurfacePlane &plane, span<u8> linear) {
        size_t size{GetSurfacePlaneSize(plane)};
        if (linear.size() < static_cast<size_t>(plane.pitch) * plane.height)
            throw std::invalid_argument("Video plane source is too small");
        ValidateSurfacePlane(state, plane);

        if (!plane.blockLinear) {
            state.soc->smmu.Write(static_cast<u32>(plane.address), linear.first(size));
            return;
        }

        std::vector<u8> swizzled(size);
        gpu::texture::CopyPitchToBlockLinear({plane.pitch, plane.height, 1}, 1, 1, 1, plane.pitch,
                                            1UL << plane.blockHeightLog2, 1, linear.data(), swizzled.data());
        state.soc->smmu.Write(static_cast<u32>(plane.address), span<u8>(swizzled));
    }
}
