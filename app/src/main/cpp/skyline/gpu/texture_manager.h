// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "texture/texture.h"
#include "texture/storage.h"
#include "texture/mapping_cache.h"

namespace skyline::gpu {
    /**
     * @brief The Texture Manager is responsible for maintaining a global view of textures being mapped from the guest to the host, any lookups and creation of host texture from equivalent guest textures alongside reconciliation of any overlaps with existing textures
     */
    class TextureManager {
      private:
        GPU &gpu;
        texture::TextureMappingCache mappingCache; //!< Guest texture mapping index

      public:
        TextureManager(GPU &gpu);

        /**
         * @return A pre-existing or newly created Texture object which matches the specified criteria
         * @note The texture manager **must** be locked prior to calling this
         */
        std::shared_ptr<TextureView> FindOrCreate(const GuestTexture &guestTexture, ContextTag tag = {});
    };
}
