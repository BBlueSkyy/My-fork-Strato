// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <functional>
#include <string>
#include <tsl/robin_map.h>
#include <shader_compiler/shader_info.h>
#include <gpu/texture/texture.h>
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
        u64 poolFlushGeneration{};

      public:
        span<TextureImageControl> textureHeaders;

        // Diagnostic metadata only; not a cache-validity guarantee.
        u64 GetPoolIova() const { return engine->texHeaderPool.offset; }
        u64 GetPoolFlushGeneration() const { return poolFlushGeneration; }

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
                       std::array<std::shared_ptr<TextureView>, Shader::NUM_TEXTURE_TYPES>,
                       util::ObjectHash<TextureImageControl>> textureHeaderStore;

        vk::raii::BufferView *GetOrCreateTexelBufferView(InterconnectContext &ctx, CachedMappedBufferView &cachedView, vk::Format format);

        struct CacheEntry {
            TextureImageControl tic;
            Shader::TextureType shaderType{};
            TextureView *view{};
            u64 sequenceNumber{};
        };
        std::vector<CacheEntry> textureHeaderCache;

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
