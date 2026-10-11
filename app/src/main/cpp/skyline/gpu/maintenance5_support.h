// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato Team and Contributors

#pragma once

namespace skyline::gpu {
    struct Maintenance5Support {
        bool createRenderpass2Extension{};
        bool depthStencilResolveExtension{};
        bool dynamicRenderingExtension{};
        bool maintenance5Extension{};
        bool maintenance5Feature{};

        constexpr bool CanEnable() const {
            return createRenderpass2Extension &&
                depthStencilResolveExtension &&
                dynamicRenderingExtension &&
                maintenance5Extension &&
                maintenance5Feature;
        }
    };
}
