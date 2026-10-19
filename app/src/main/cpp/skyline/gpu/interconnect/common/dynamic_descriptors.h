// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <memory>
#include <shader_compiler/shader_info.h>
#include "common.h"

namespace skyline::gpu::interconnect {
    class Textures;
    class Samplers;

    /** GPU-selected TIC/TSC tables. Sets and metadata remain immutable while in use. */
    class DynamicDescriptors {
        struct Impl;
        std::unique_ptr<Impl> impl;
      public:
        DynamicDescriptors(InterconnectContext &ctx, Textures &textures, Samplers &samplers,
                           span<const Shader::DynamicTextureDescriptor> groups);
        ~DynamicDescriptors();
        DynamicDescriptors(const DynamicDescriptors &) = delete;
        DynamicDescriptors &operator=(const DynamicDescriptors &) = delete;
        vk::DescriptorSetLayout Layout() const;
        void ValidateLimits(InterconnectContext &ctx, u32 staticSampled, u32 staticSamplers, u32 staticResources, u32 staticStorage = 0) const;
        bool CanRepresent(InterconnectContext &ctx, Textures &textures, Samplers &samplers);
        vk::DescriptorSet Sync(InterconnectContext &ctx, Textures &textures, Samplers &samplers,
                               vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask);
    };
}
