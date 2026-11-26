// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

#include <cstdint>
#include <services/am/applet/IApplet.h>
#include <services/am/applet/indirect_layer_registry.h>
#include "indirect_layer_layout.h"
#include "results.h"

namespace skyline::service::visrv {
    /** @brief Raw-data fields shared by GetIndirectLayerImageMap (2450) and GetIndirectLayerImageCropMap (2451) */
    struct IndirectLayerImageArguments {
        std::int64_t width{};
        std::int64_t height{};
        std::uint64_t handle{};
        std::uint64_t appletResourceUserId{};
    };

    /** @brief 2450 raw data: s64 width, s64 height, u64 consumer handle, u64 AppletResourceUserId */
    template<typename Request>
    IndirectLayerImageArguments ReadIndirectLayerImageMapArguments(Request &request) {
        IndirectLayerImageArguments arguments;
        arguments.width = request.template Pop<std::int64_t>();
        arguments.height = request.template Pop<std::int64_t>();
        arguments.handle = request.template Pop<std::uint64_t>();
        arguments.appletResourceUserId = request.template Pop<std::uint64_t>();
        return arguments;
    }

    /** @brief 2451 raw data: four packed f32 crop values, then the same four u64 fields as 2450 (the last is the AppletResourceUserId) */
    template<typename Request>
    IndirectLayerImageArguments ReadIndirectLayerImageCropMapArguments(Request &request) {
        for (int index{}; index < 4; index++)
            request.template Pop<float>();
        IndirectLayerImageArguments arguments;
        arguments.width = static_cast<std::int64_t>(request.template Pop<std::uint64_t>());
        arguments.height = static_cast<std::int64_t>(request.template Pop<std::uint64_t>());
        arguments.handle = request.template Pop<std::uint64_t>();
        arguments.appletResourceUserId = request.template Pop<std::uint64_t>();
        return arguments;
    }

    /**
     * @brief Serialises the GetIndirectLayerImageRequiredMemoryInfo (2460) reply: s64 size, then s64 alignment
     * @url https://switchbrew.org/wiki/Display_services#GetIndirectLayerImageRequiredMemoryInfo
     * @note The size is the buffer size callers must pass to the ImageMap commands, so it must never be
     *       smaller than the image those commands write (see MapIndirectLayerImage)
     */
    template<typename Response>
    void WriteIndirectLayerMemoryInfoReply(Response &response, const IndirectLayerLayout &layout) {
        response.template Push<std::int64_t>(static_cast<std::int64_t>(layout.requiredSize));
        response.template Push<std::int64_t>(static_cast<std::int64_t>(IndirectLayerAlignment));
    }

    /**
     * @brief Serialises the GetIndirectLayerImageMap (2450) / GetIndirectLayerImageCropMap (2451) reply: s64 size, then s64 stride
     * @url https://switchbrew.org/wiki/Display_services#GetIndirectLayerImageMap
     */
    template<typename Response>
    void WriteIndirectLayerImageMapReply(Response &response, const IndirectLayerLayout &layout) {
        response.template Push<std::int64_t>(static_cast<std::int64_t>(layout.imageSize));
        response.template Push<std::int64_t>(static_cast<std::int64_t>(layout.stride));
    }

    /**
     * @brief Validates an ImageMap/CropMap request and renders the indirect layer into the guest buffer
     * @note Validation order matches the original handler: dimensions, buffer size/alignment, layer handle, image availability
     * @return Success with `layout` filled in, otherwise the failing VI result (the reply must then not be serialised)
     */
    inline Result MapIndirectLayerImage(am::IndirectLayerRegistry &registry, std::uint64_t processId,
                                        std::int64_t width, std::int64_t height,
                                        std::uint64_t handle, std::uint64_t appletResourceUserId,
                                        span<u8> imageBuffer, IndirectLayerLayout &layout) {
        if (!CalculateIndirectLayerLayout(width, height, layout))
            return result::InvalidDimensions;

        if (imageBuffer.size() < layout.imageSize || reinterpret_cast<std::uintptr_t>(imageBuffer.data()) % IndirectLayerAlignment)
            return result::InvalidArgument;

        const auto applet{registry.Get(handle, processId, appletResourceUserId)};
        if (!applet)
            return result::InvalidValue;

        if (!applet->GetIndirectLayerImage(imageBuffer.first(layout.imageSize)))
            return result::NoData;

        return {};
    }
}
