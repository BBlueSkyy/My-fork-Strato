// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include <gpu/pipeline_cache_manager.h>
#include "graphics_pipeline_state_accessor.h"
#include "../common/pipeline.inc"



namespace skyline::gpu::interconnect::maxwell3d {
    RuntimeGraphicsPipelineStateAccessor::RuntimeGraphicsPipelineStateAccessor(std::unique_ptr<PipelineStateBundle> bundle,
                                                                               InterconnectContext &ctx,
                                                                               Textures &textures, Samplers &samplers, ConstantBufferSet &constantBuffers,
                                                                               const std::array<ShaderBinary, engine::PipelineCount> &shaderBinaries)
        : bundle{std::move(bundle)}, ctx{ctx}, textures{textures}, samplers{samplers}, constantBuffers{constantBuffers}, shaderBinaries{shaderBinaries} {}


    Shader::TextureType RuntimeGraphicsPipelineStateAccessor::GetTextureType(u32 index) const {
        Shader::TextureType type{textures.GetTextureType(ctx, index)};
        bundle->AddTextureType(index, type);
        return type;
    }

    Shader::CompareFunction RuntimeGraphicsPipelineStateAccessor::GetTextureCompareFunc(u32 index) const {
        BindlessHandle handle{.raw = index};
        auto function{samplers.GetTextureCompareFunc(ctx, handle.samplerIndex, handle.textureIndex)};
        bundle->AddTextureCompareFunction(index, function);
        return function;
    }

    Shader::TextureSwizzleMapping RuntimeGraphicsPipelineStateAccessor::GetTextureSwizzle(u32 index) const {
        auto mapping{textures.GetTextureBufferSwizzle(ctx, index)};
        bundle->AddTextureSwizzle(index, mapping);
        return mapping;
    }

    u32 RuntimeGraphicsPipelineStateAccessor::GetConstantBufferValue(u32 shaderStage, u32 index, u32 offset) const {
        u32 value{constantBuffers[shaderStage][index].Read<u32>(ctx.executor, offset)};
        bundle->AddConstantBufferValue(shaderStage, index, offset, value);
        return value;
    }

    ShaderBinary RuntimeGraphicsPipelineStateAccessor::GetShaderBinary(u32 pipelineStage) const {
        ShaderBinary binary{shaderBinaries[pipelineStage]};
        bundle->SetShaderBinary(pipelineStage, binary);
        return binary;
    }

    void RuntimeGraphicsPipelineStateAccessor::MarkComplete() {
        if (ctx.gpu.graphicsPipelineCacheManager)
            ctx.gpu.graphicsPipelineCacheManager->QueueWrite(std::move(bundle));
    }
}
