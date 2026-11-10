// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <functional>
#include <string>
#include <tsl/robin_map.h>
#include <shader_compiler/shader_info.h>
#include <gpu/texture/texture.h>
#include <gpu/texture/mapping_cache.h>
#include "common.h"
#include "null_texture_policy.h"
#include "tic.h"

namespace skyline::gpu::interconnect {
    class TexturePoolState : dirty::CachedManualDirty {
      public:
        struct EngineRegisters {
            const engine_common::TexHeaderPool &texHeaderPool;

            void DirtyBind(DirtyManager &manager, dirty::Handle handle) const;
        };

      private:
        dirty::BoundSubresource<EngineRegisters> engine;

      public:
        span<TextureImageControl> textureHeaders;
        MappingIdentity::Snapshot mapping;

        TexturePoolState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine);

        void Flush(InterconnectContext &ctx);

        void PurgeCaches();
    };

    class Textures {
      private:
        std::shared_ptr<TextureView> nullTextureView{};
        dirty::ManualDirtyState<TexturePoolState> texturePool;

        // One TIC can legally be consumed through different shader-visible view
        // types. Keep those views separate while TextureManager still shares the
        // underlying VkImage backing.
        tsl::robin_map<TextureImageControl,
                       std::array<texture::MappingCacheEntry<TextureView>, Shader::NUM_TEXTURE_TYPES>,
                       util::ObjectHash<TextureImageControl>> textureHeaderStore;

        vk::raii::BufferView *GetOrCreateTexelBufferView(InterconnectContext &ctx, CachedMappedBufferView &cachedView, vk::Format format);

        struct CacheEntry {
            TextureImageControl tic;
            Shader::TextureType shaderType{};
            texture::MappingCacheEntry<TextureView> texture;
            u64 sequenceNumber{};
        };
        std::vector<CacheEntry> textureHeaderCache;
        span<TextureImageControl> GetTextureHeaders(InterconnectContext &ctx);

      public:
        Textures(DirtyManager &manager, const TexturePoolState::EngineRegisters &engine);

        void MarkAllDirty();

        /**
         * @param nullReason If non-null, receives why the shared null texture was returned (NullTextureReason::None when a guest-backed view was returned)
         */
        TextureView *GetTexture(InterconnectContext &ctx, u32 index, Shader::TextureType shaderType, NullTextureReason *nullReason = nullptr);

        Shader::TextureType GetTextureType(InterconnectContext &ctx, u32 index);

        bool IsTexturePixelFormatInteger(InterconnectContext &ctx, u32 index);

        vk::raii::BufferView *GetTextureBufferView(InterconnectContext &ctx, u32 index, CachedMappedBufferView &cachedView);

        vk::raii::BufferView *GetImageBufferView(InterconnectContext &ctx, u32 index, Shader::ImageFormat format, CachedMappedBufferView &cachedView);
    };
}
