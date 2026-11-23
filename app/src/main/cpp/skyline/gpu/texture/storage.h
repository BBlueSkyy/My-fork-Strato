// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)
#pragma once

#include <memory>
#include <utility>
#include "guest_range.h"

namespace skyline::gpu {
    class Texture;
    namespace texture {
        /**
         * Transitional V2 owner and immutable guest identity. Texture continues to
         * own its backing, fences and V3 AliasGroup; no second authority is added.
         */
        class TextureStorage {
          public:
            const std::shared_ptr<Texture> texture;
            const GuestResourceRanges ranges;

            TextureStorage(std::shared_ptr<Texture> texture, GuestResourceRanges ranges)
                : texture(std::move(texture)), ranges(std::move(ranges)) {}
        };
    }
}
