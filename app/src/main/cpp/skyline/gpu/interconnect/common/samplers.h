// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <tsl/robin_map.h>
#include "common.h"
#include "tsc.h"
#include <shader_compiler/runtime_info.h>

namespace skyline::gpu::interconnect {
    class SamplerPoolState : dirty::CachedManualDirty, dirty::RefreshableManualDirty {
      public:
        struct EngineRegisters {
            const engine_common::TexSamplerPool &texSamplerPool;
            const engine_common::TexHeaderPool &texHeaderPool;

            void DirtyBind(DirtyManager &manager, dirty::Handle handle) const;
        };

      private:
        dirty::BoundSubresource<EngineRegisters> engine;

      public:
        span<TextureSamplerControl> texSamplers;
        bool didUseTexHeaderBinding;
        u64 bindingGeneration{};
        std::vector<TextureSamplerControl> dynamicHeaders;

        SamplerPoolState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine);

        void Flush(InterconnectContext &ctx, bool useTexHeaderBinding);
        span<const TextureSamplerControl> ReadDynamicHeaders(InterconnectContext &ctx);

        bool Refresh(InterconnectContext &ctx, bool useTexHeaderBinding);

        void PurgeCaches();
    };

    class Samplers {
      private:
        dirty::ManualDirtyState<SamplerPoolState> samplerPool;

        tsl::robin_map<TextureSamplerControl, std::shared_ptr<vk::raii::Sampler>, util::ObjectHash<TextureSamplerControl>> texSamplerStore;
        std::vector<vk::raii::Sampler *> texSamplerCache;
        const TextureSamplerControl *texSamplerCachePool{};

      public:
        Samplers(DirtyManager &manager, const SamplerPoolState::EngineRegisters &engine);

        void Update(InterconnectContext &ctx, bool useTexHeaderBinding);

        u64 GetBindingGeneration() { return samplerPool.Get().bindingGeneration; }

        void MarkAllDirty();

        span<const TextureSamplerControl> GetPoolHeaders(InterconnectContext &ctx);
        bool IsViaHeaderBinding() { return samplerPool.Get().didUseTexHeaderBinding; }
        std::shared_ptr<vk::raii::Sampler> GetDynamicSampler(InterconnectContext &ctx, u32 index);
        bool DynamicSamplerUsesLinear(u32 index);

        vk::raii::Sampler *GetSampler(InterconnectContext &ctx, u32 samplerIndex, u32 textureIndex);

        // Explicit TSC lookup for fixed-function methods such as DRAW_TEXTURE.
        // The three-argument overload retains programmable shader binding rules.
        vk::raii::Sampler *GetSampler(InterconnectContext &ctx, u32 samplerIndex);

        Shader::CompareFunction GetTextureCompareFunc(InterconnectContext &ctx, u32 samplerIndex, u32 textureIndex);
    };
}
