// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <tsl/robin_map.h>
#include <boost/container/small_vector.hpp>
#include <shader_compiler/frontend/ir/program.h>
#include <gpu/interconnect/common/samplers.h>
#include <gpu/interconnect/common/textures.h>
#include <gpu/interconnect/common/pipeline_specialization.h>
#include "packed_pipeline_state.h"
#include "constant_buffers.h"

namespace skyline::gpu {
    class TextureView;
}

namespace skyline::gpu::interconnect::kepler_compute {
    class Pipeline {
      public:
        struct ShaderStage {
            vk::ShaderModule module;
            Shader::Info info;
        };

        struct DescriptorInfo {
            std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;

            u32 totalWriteDescCount;
            u32 totalBufferDescCount;
            u32 totalTexelBufferDescCount;
            u32 totalImageDescCount;
        };

        struct CompiledPipeline {
            vk::raii::DescriptorSetLayout descriptorSetLayout;
            vk::raii::PipelineLayout pipelineLayout;
            vk::raii::Pipeline pipeline;
            bool usesPushDescriptors{};
        };

      private:
        PipelineSpecialization specialization;
        ShaderStage shaderStage;
        DescriptorInfo descriptorInfo;
        std::vector<CachedMappedBufferView> storageBufferViews;
        std::vector<CachedMappedBufferView> texelBufferViews;
        ContextTag lastExecutionTag{}; //!< The last execution tag this pipeline was used at

        void SyncCachedStorageBufferViews(ContextTag executionTag);

      public:
        CompiledPipeline compiledPipeline;

        PackedPipelineState sourcePackedState;

        Pipeline(InterconnectContext &ctx, Textures &textures, Samplers &samplers, ConstantBufferSet &constantBuffers, const PackedPipelineState &packedState, const ShaderBinary &shaderBinary);

        bool MatchesSpecialization(InterconnectContext &ctx, Textures &textures, Samplers &samplers, ConstantBufferSet &constantBuffers) const;

        /**
         * @brief Creates a descriptor set update from the current GPU state
         */
        DescriptorUpdateInfo *SyncDescriptors(InterconnectContext &ctx, ConstantBufferSet &constantBuffers, Samplers &samplers, Textures &textures, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask, const std::array<u32, 3> &groups, boost::container::small_vector<TextureView *, 8> &pendingWrites);
    };

    class PipelineManager {
      private:
        tsl::robin_map<PackedPipelineState, std::vector<std::unique_ptr<Pipeline>>, util::ObjectHash<PackedPipelineState>> map;

      public:
        Pipeline *FindOrCreate(InterconnectContext &ctx, Textures &textures, Samplers &samplers, ConstantBufferSet &constantBuffers, const PackedPipelineState &packedState, const ShaderBinary &shaderBinary) {
            auto it{map.find(packedState)};
            if (it != map.end())
                for (auto variant{it->second.rbegin()}; variant != it->second.rend(); ++variant)
                    if ((*variant)->MatchesSpecialization(ctx, textures, samplers, constantBuffers))
                        return variant->get();

            auto candidate{std::make_unique<Pipeline>(ctx, textures, samplers, constantBuffers, packedState, shaderBinary)};
            auto *pipeline{candidate.get()};
            map[packedState].push_back(std::move(candidate));
            return pipeline;
        }
    };
}
