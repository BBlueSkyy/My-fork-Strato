// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <algorithm>
#include <gpu/texture/texture.h>
#include <gpu/interconnect/command_executor.h>
#include <gpu/interconnect/common/pipeline.inc>
#include <gpu/shader_manager.h>
#include <gpu.h>
#include "pipeline_manager.h"

namespace skyline::gpu::interconnect::kepler_compute {
    static Pipeline::ShaderStage MakePipelineShader(InterconnectContext &ctx, Textures &textures, Samplers &samplers, ConstantBufferSet &constantBuffers, const PackedPipelineState &packedState, const ShaderBinary &shaderBinary) {
        ctx.gpu.shader->ResetPools();

        auto program{ctx.gpu.shader->ParseComputeShader(
            packedState.shaderHash, shaderBinary.binary, shaderBinary.baseOffset,
            packedState.bindlessTextureConstantBufferSlotSelect,
            packedState.localMemorySize, packedState.sharedMemorySize,
            packedState.dimensions,
            [&](u32 index, u32 offset) {
                return constantBuffers[index].ReadForShaderCompile<int>(ctx.executor, offset);
            }, [&](u32 index) {
                return textures.GetTextureType(ctx, BindlessHandle{ .raw = index }.textureIndex);
            }, [&](u32 index) {
                return textures.IsTexturePixelFormatInteger(ctx, BindlessHandle{ .raw = index }.textureIndex);
            }, [&](u32 index) {
                BindlessHandle handle{ .raw = index };
                return samplers.GetTextureCompareFunc(ctx, handle.samplerIndex, handle.textureIndex);
            })};

        const auto diagnosticPipeline{util::ObjectHash<PackedPipelineState>{}(packedState)};
        const bool diagnosticRelevant{std::any_of(program.info.storage_buffers_descriptors.begin(),
            program.info.storage_buffers_descriptors.end(), [](const auto &desc) { return !desc.has_size; })};
        if (diagnosticRelevant && ssbo_diagnostics::compiledPipelines.fetch_add(1, std::memory_order_relaxed) < 128) {
            LOGINF("SsboTrace pipeline shader=0x{:016X} pipeline=0x{:016X} local={},{},{} local_memory={} shared_memory={}",
                packedState.shaderHash, diagnosticPipeline, packedState.dimensions[0], packedState.dimensions[1], packedState.dimensions[2],
                packedState.localMemorySize, packedState.sharedMemorySize);
            for (const auto &desc : program.info.storage_buffers_descriptors) {
                const auto &range{desc.access_range};
                LOGINF("SsboTrace descriptor shader=0x{:016X} pipeline=0x{:016X} cbuf={} offset=0x{:X} sized={} written={} known={} constant={} group_coeff={},{},{} local_coeff={},{},{}",
                    packedState.shaderHash, diagnosticPipeline, desc.cbuf_index, desc.cbuf_offset, desc.has_size, desc.is_written, range.known,
                    range.constant, range.workgroup[0], range.workgroup[1], range.workgroup[2], range.local[0], range.local[1], range.local[2]);
            }
            for (const auto &failure : program.info.compute_ssbo_diagnostics) {
                constexpr size_t ChunkSize{1400};
                for (size_t offset{}; offset < failure.detail.size(); offset += ChunkSize)
                    LOGINF("SsboTrace inference shader=0x{:016X} pipeline=0x{:016X} cbuf={} offset=0x{:X} part={} total={} detail={}",
                        packedState.shaderHash, diagnosticPipeline, failure.cbuf_index, failure.cbuf_offset, offset / ChunkSize,
                        (failure.detail.size() + ChunkSize - 1) / ChunkSize, failure.detail.substr(offset, ChunkSize));
            }
        } else if (diagnosticRelevant) ssbo_diagnostics::diagnosticPipelinesDropped.fetch_add(1, std::memory_order_relaxed);
        // Diagnostic strings are not retained in pipeline runtime state.
        program.info.compute_ssbo_diagnostics.clear();
        Shader::Backend::Bindings bindings{};

        return {ctx.gpu.shader->CompileShader({}, program, bindings, packedState.shaderHash), program.info};
    }

    static Pipeline::DescriptorInfo MakePipelineDescriptorInfo(const Pipeline::ShaderStage &stage) {
        Pipeline::DescriptorInfo descriptorInfo{};
        u32 bindingIndex{};

        auto pushBindings{[&](vk::DescriptorType type, const auto &descs, u32 &count) {
            descriptorInfo.totalWriteDescCount += descs.size();

            for (u32 descIdx{}; descIdx < descs.size(); descIdx++) {
                const auto &desc{descs[descIdx]};
                count += desc.count;

                descriptorInfo.descriptorSetLayoutBindings.push_back(vk::DescriptorSetLayoutBinding{
                    .binding = bindingIndex++,
                    .descriptorType = type,
                    .descriptorCount = desc.count,
                    .stageFlags = vk::ShaderStageFlagBits::eCompute,
                });
            }
        }};

        pushBindings(vk::DescriptorType::eUniformBuffer, stage.info.constant_buffer_descriptors, descriptorInfo.totalBufferDescCount);
        pushBindings(vk::DescriptorType::eStorageBuffer, stage.info.storage_buffers_descriptors, descriptorInfo.totalBufferDescCount);

        pushBindings(vk::DescriptorType::eUniformTexelBuffer, stage.info.texture_buffer_descriptors, descriptorInfo.totalTexelBufferDescCount);
        pushBindings(vk::DescriptorType::eStorageTexelBuffer, stage.info.image_buffer_descriptors, descriptorInfo.totalTexelBufferDescCount);

        pushBindings(vk::DescriptorType::eCombinedImageSampler, stage.info.texture_descriptors, descriptorInfo.totalImageDescCount);
        pushBindings(vk::DescriptorType::eStorageImage, stage.info.image_descriptors, descriptorInfo.totalImageDescCount);

        return descriptorInfo;
    }

    static Pipeline::CompiledPipeline MakeCompiledPipeline(InterconnectContext &ctx,
                                                                               const PackedPipelineState &packedState,
                                                                               const Pipeline::ShaderStage &shaderStage,
                                                                               span<vk::DescriptorSetLayoutBinding> layoutBindings) {
        const bool usesPushDescriptors{ctx.gpu.traits.CanUsePushDescriptors(layoutBindings)};
        vk::raii::DescriptorSetLayout descriptorSetLayout{ctx.gpu.vkDevice, vk::DescriptorSetLayoutCreateInfo{
            .flags = vk::DescriptorSetLayoutCreateFlags{usesPushDescriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR : vk::DescriptorSetLayoutCreateFlags{}},
            .pBindings = layoutBindings.data(),
            .bindingCount = static_cast<u32>(layoutBindings.size()),
        }};

        vk::raii::PipelineLayout pipelineLayout{ctx.gpu.vkDevice, vk::PipelineLayoutCreateInfo{
            .pSetLayouts = &*descriptorSetLayout,
            .setLayoutCount = 1,
        }};

        vk::PipelineShaderStageCreateInfo shaderStageInfo{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = &*shaderStage.module,
            .pName = "main"
        };

        vk::ComputePipelineCreateInfo pipelineInfo{
            .stage = shaderStageInfo,
            .layout = *pipelineLayout,
        };


        if (ctx.gpu.traits.quirks.brokenMultithreadedPipelineCompilation)
            ctx.gpu.graphicsPipelineAssembler->WaitIdle();

        vk::raii::Pipeline pipeline{ctx.gpu.vkDevice, nullptr, pipelineInfo};

        return Pipeline::CompiledPipeline{
            .descriptorSetLayout = std::move(descriptorSetLayout),
            .pipelineLayout = std::move(pipelineLayout),
            .pipeline = std::move(pipeline),
            .usesPushDescriptors = usesPushDescriptors,
        };
    }

    Pipeline::Pipeline(InterconnectContext &ctx, Textures &textures, Samplers &samplers, ConstantBufferSet &constantBuffers, const PackedPipelineState &packedState, const ShaderBinary &shaderBinary)
        : shaderStage{MakePipelineShader(ctx, textures, samplers, constantBuffers, packedState, shaderBinary)},
          descriptorInfo{MakePipelineDescriptorInfo(shaderStage)},
          compiledPipeline{MakeCompiledPipeline(ctx, packedState, shaderStage, descriptorInfo.descriptorSetLayoutBindings)},
          sourcePackedState{packedState} {
        size_t storageBufferCount{};
        for (const auto &desc : shaderStage.info.storage_buffers_descriptors)
            storageBufferCount += desc.count;

        storageBufferViews.resize(storageBufferCount);
        texelBufferViews.resize(descriptorInfo.totalTexelBufferDescCount);
    }

    void Pipeline::SyncCachedStorageBufferViews(ContextTag executionTag) {
        if (lastExecutionTag != executionTag) {
            for (auto &view : storageBufferViews)
                view.PurgeCaches();
            for (auto &view : texelBufferViews)
                view.PurgeCaches();

            lastExecutionTag = executionTag;
        }
    }

    DescriptorUpdateInfo *Pipeline::SyncDescriptors(InterconnectContext &ctx, ConstantBufferSet &constantBuffers, Samplers &samplers, Textures &textures, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask, const std::array<u32, 3> &groups) {
        SyncCachedStorageBufferViews(ctx.executor.executionTag);

        u32 writeIdx{};
        auto writes{ctx.executor.allocator->AllocateUntracked<vk::WriteDescriptorSet>(descriptorInfo.totalWriteDescCount)};

        u32 bufferIdx{};
        auto bufferDescs{ctx.executor.allocator->AllocateUntracked<vk::DescriptorBufferInfo>(descriptorInfo.totalBufferDescCount)};
        auto bufferDescDynamicBindings{ctx.executor.allocator->AllocateUntracked<DynamicBufferBinding>(descriptorInfo.totalBufferDescCount)};
        u32 imageIdx{};
        auto imageDescs{ctx.executor.allocator->AllocateUntracked<vk::DescriptorImageInfo>(descriptorInfo.totalImageDescCount)};
        u32 texelBufferIdx{};
        auto texelBufferDescs{ctx.executor.allocator->AllocateUntracked<vk::BufferView>(descriptorInfo.totalTexelBufferDescCount)};

        u32 storageBufferIdx{};
        u32 texelBufferCacheIdx{};
        u32 bindingIdx{};

        /**
         * @brief Adds descriptor writes for a single Vulkan descriptor type that uses buffer descriptors
         * @param count Total number of descriptors to write, including array elements
         */
        auto writeBufferDescs{[&](vk::DescriptorType type, const auto &descs, auto getBufferCb) {
            if (!descs.empty()) {
                // The underlying buffer bindings will be resolved from the dynamic ones during recording
                for (const auto &desc : descs) {
                    writes[writeIdx++] = {
                        .dstBinding = bindingIdx++,
                        .descriptorCount = desc.count,
                        .descriptorType = type,
                        .pBufferInfo = &bufferDescs[bufferIdx],
                    };

                    for (u32 arrayIdx{}; arrayIdx < desc.count; arrayIdx++)
                        bufferDescDynamicBindings[bufferIdx++] = getBufferCb(desc, arrayIdx);
                }
            }
        }};

        auto writeImageDescs{[&](vk::DescriptorType type, const auto &descs, auto getTextureCb) {
            if (!descs.empty()) {
                for (const auto &desc : descs) {
                    writes[writeIdx++] = {
                        .dstBinding = bindingIdx++,
                        .descriptorCount = desc.count,
                        .descriptorType = type,
                        .pImageInfo = &imageDescs[imageIdx],
                    };

                    for (u32 arrayIdx{}; arrayIdx < desc.count; arrayIdx++)
                        imageDescs[imageIdx++] = getTextureCb(desc, arrayIdx);
                }
            }
        }};

        auto writeTexelBufferDescs{[&](vk::DescriptorType type, const auto &descs, auto getBufferViewCb) {
            for (const auto &desc : descs) {
                writes[writeIdx++] = {
                    .dstBinding = bindingIdx++,
                    .descriptorCount = desc.count,
                    .descriptorType = type,
                    .pTexelBufferView = &texelBufferDescs[texelBufferIdx],
                };

                for (u32 arrayIdx{}; arrayIdx < desc.count; arrayIdx++)
                    texelBufferDescs[texelBufferIdx++] = getBufferViewCb(desc, arrayIdx);
            }
        }};

        writeBufferDescs(vk::DescriptorType::eUniformBuffer, shaderStage.info.constant_buffer_descriptors,
                         [&](const Shader::ConstantBufferDescriptor &desc, size_t arrayIdx) {
                             size_t cbufIdx{desc.index + arrayIdx};
                             return GetConstantBufferBinding(ctx, shaderStage.info.constant_buffer_used_sizes,
                                                             constantBuffers[cbufIdx].view, cbufIdx,
                                                             vk::PipelineStageFlagBits::eComputeShader,
                                                             srcStageMask, dstStageMask);
                         });

        writeBufferDescs(vk::DescriptorType::eStorageBuffer, shaderStage.info.storage_buffers_descriptors,
                         [&](const Shader::StorageBufferDescriptor &desc, size_t) {
                             auto &cachedView{storageBufferViews[storageBufferIdx++]};
                             const auto accessSize{desc.access_range.GetSize(groups, sourcePackedState.dimensions)};
                             const ssbo_diagnostics::Dispatch diagnostic{sourcePackedState.shaderHash,
                                 util::ObjectHash<PackedPipelineState>{}(sourcePackedState), groups};
                             auto binding{GetStorageBufferBinding(ctx, desc, constantBuffers[desc.cbuf_index],
                                                                  cachedView,
                                                                  vk::PipelineStageFlagBits::eComputeShader,
                                                                  srcStageMask, dstStageMask,
                                                                  accessSize, &diagnostic)};
                             return binding;
                         });

        writeTexelBufferDescs(vk::DescriptorType::eUniformTexelBuffer, shaderStage.info.texture_buffer_descriptors,
                              [&](const Shader::TextureBufferDescriptor &desc, size_t arrayIdx) {
                                  BindlessHandle handle{ReadBindlessHandle(ctx, constantBuffers, desc, arrayIdx)};
                                  return GetTextureBufferBinding(ctx, textures, handle,
                                                                 texelBufferViews[texelBufferCacheIdx++],
                                                                 vk::PipelineStageFlagBits::eComputeShader,
                                                                 srcStageMask, dstStageMask);
                              });

        writeTexelBufferDescs(vk::DescriptorType::eStorageTexelBuffer, shaderStage.info.image_buffer_descriptors,
                              [&](const Shader::ImageBufferDescriptor &desc, size_t arrayIdx) {
                                  BindlessHandle handle{ReadBindlessHandle(ctx, constantBuffers, desc, arrayIdx)};
                                  return GetImageBufferBinding(ctx, textures, handle, desc.format, desc.is_written,
                                                               texelBufferViews[texelBufferCacheIdx++],
                                                               vk::PipelineStageFlagBits::eComputeShader,
                                                               srcStageMask, dstStageMask);
                              });

        writeImageDescs(vk::DescriptorType::eCombinedImageSampler, shaderStage.info.texture_descriptors,
                        [&](const Shader::TextureDescriptor &desc, size_t arrayIdx) {
                            BindlessHandle handle{ReadBindlessHandle(ctx, constantBuffers, desc, arrayIdx)};
                            auto binding{GetTextureBinding(ctx, desc,
                                                           samplers, textures, handle,
                                                           vk::PipelineStageFlagBits::eComputeShader,
                                                           srcStageMask, dstStageMask)};
                            return binding.first;
                        });
         
        writeImageDescs(vk::DescriptorType::eStorageImage, shaderStage.info.image_descriptors,
                         [&](const Shader::ImageDescriptor &desc, size_t arrayIdx) {
                             BindlessHandle handle{ReadBindlessHandle(ctx, constantBuffers, desc, arrayIdx)};
                             LOGD("StorageImage: idx={} format={}", static_cast<u32>(handle.textureIndex), static_cast<u32>(desc.format));
                             auto binding{GetImageBinding(ctx, desc, textures, handle,
                                                          vk::PipelineStageFlagBits::eComputeShader,
                                                          srcStageMask, dstStageMask)};
                            return binding.first;
                        });

        if (!writeIdx)
            return nullptr;

        return ctx.executor.allocator->EmplaceUntracked<DescriptorUpdateInfo>(DescriptorUpdateInfo{
            .writes = writes.first(writeIdx),
            .bufferDescs = bufferDescs.first(bufferIdx),
            .bufferDescDynamicBindings = bufferDescDynamicBindings.first(bufferIdx),
            .pipelineLayout = *compiledPipeline.pipelineLayout,
            .descriptorSetLayout = *compiledPipeline.descriptorSetLayout,
            .bindPoint = vk::PipelineBindPoint::eCompute,
            .descriptorSetIndex = 0,
        });
    }
}
