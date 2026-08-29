// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <functional>
#include <tsl/robin_map.h>
#include <shader_compiler/shader_info.h>
#include <gpu/texture/texture.h>
#include "common.h"
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
        struct TexelBufferViewKey {
            VkBuffer buffer;
            VkDeviceSize offset;
            VkDeviceSize range;
            VkFormat format;

            bool operator==(const TexelBufferViewKey &) const = default;
        };

        struct TexelBufferViewKeyHash {
            size_t operator()(const TexelBufferViewKey &key) const {
                size_t hash{std::hash<VkBuffer>{}(key.buffer)};
                hash ^= std::hash<VkDeviceSize>{}(key.offset) + 0x9E3779B9 + (hash << 6) + (hash >> 2);
                hash ^= std::hash<VkDeviceSize>{}(key.range) + 0x9E3779B9 + (hash << 6) + (hash >> 2);
                hash ^= std::hash<VkFormat>{}(key.format) + 0x9E3779B9 + (hash << 6) + (hash >> 2);
                return hash;
            }
        };

        std::shared_ptr<TextureView> nullTextureView{};
        dirty::ManualDirtyState<TexturePoolState> texturePool;

        tsl::robin_map<TextureImageControl, std::shared_ptr<TextureView>, util::ObjectHash<TextureImageControl>> textureHeaderStore;
        tsl::robin_map<TexelBufferViewKey, std::unique_ptr<vk::raii::BufferView>, TexelBufferViewKeyHash> texelBufferViewStore;

        vk::raii::BufferView *GetOrCreateTexelBufferView(InterconnectContext &ctx, CachedMappedBufferView &cachedView, vk::Format format);

        struct CacheEntry {
            TextureImageControl tic;
            TextureView *view;
            u64 sequenceNumber;
        };
        std::vector<CacheEntry> textureHeaderCache;

      public:
        Textures(DirtyManager &manager, const TexturePoolState::EngineRegisters &engine);

        void MarkAllDirty();

        TextureView *GetTexture(InterconnectContext &ctx, u32 index, Shader::TextureType shaderType);

        Shader::TextureType GetTextureType(InterconnectContext &ctx, u32 index);

        vk::raii::BufferView *GetTextureBufferView(InterconnectContext &ctx, u32 index, CachedMappedBufferView &cachedView);

        vk::raii::BufferView *GetImageBufferView(InterconnectContext &ctx, u32 index, Shader::ImageFormat format,
                                                 bool isWritten, CachedMappedBufferView &cachedView);
    };
}
