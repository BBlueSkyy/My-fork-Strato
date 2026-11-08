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

      public:
        span<TextureImageControl> textureHeaders;

        TexturePoolState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine);

        void Flush(InterconnectContext &ctx);

        void PurgeCaches();
    };

    class Textures {
      private:
        std::shared_ptr<TextureView> nullTextureView{};
        dirty::ManualDirtyState<TexturePoolState> texturePool;

        tsl::robin_map<TextureImageControl, std::shared_ptr<TextureView>, util::ObjectHash<TextureImageControl>> textureHeaderStore;

        vk::raii::BufferView *GetOrCreateTexelBufferView(InterconnectContext &ctx, CachedMappedBufferView &cachedView, vk::Format format);

        struct CacheEntry {
            TextureImageControl tic;
            TextureView *view;
            u64 sequenceNumber;
        };
        std::vector<CacheEntry> textureHeaderCache;

        std::vector<u64> reportedUnmappedIovas; //!< IOVAs of unmapped TICs that were already logged, bounded so a dispatch loop cannot flood the log

        void ReportUnmappedTexture(u64 iova);

      public:
        Textures(DirtyManager &manager, const TexturePoolState::EngineRegisters &engine);

        void MarkAllDirty();

        /**
         * @param nullReason If non-null, receives why the shared null texture was returned (NullTextureReason::None when a guest-backed view was returned)
         */
        TextureView *GetTexture(InterconnectContext &ctx, u32 index, Shader::TextureType shaderType, NullTextureReason *nullReason = nullptr);

        /**
         * @brief Formats the TIC at `index` and its GMMU resolution for diagnostics, only intended for failure paths
         */
        std::string DescribeTexture(InterconnectContext &ctx, u32 index);

        Shader::TextureType GetTextureType(InterconnectContext &ctx, u32 index);

        bool IsTexturePixelFormatInteger(InterconnectContext &ctx, u32 index);

        vk::raii::BufferView *GetTextureBufferView(InterconnectContext &ctx, u32 index, CachedMappedBufferView &cachedView);

        vk::raii::BufferView *GetImageBufferView(InterconnectContext &ctx, u32 index, Shader::ImageFormat format, CachedMappedBufferView &cachedView);
    };
}
