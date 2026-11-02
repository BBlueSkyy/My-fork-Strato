// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <chrono>
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

        Shader::Backend::Bindings bindings{};

        Pipeline::ShaderStage stage{};
        std::function<void(span<const u32>)> capture;
        if (ctx.gpu.shader->computeDiagnostics.Enabled()) {
            capture = [&](span<const u32> spirv) {
                stage.diagnosticSpirvBytes = spirv.size_bytes();
                if (spirv.size_bytes() <= ComputeDiagnosticStore::MaxShaderBytes)
                    stage.diagnosticSpirv.assign(spirv.begin(), spirv.end());
            };
        }
        stage.module = ctx.gpu.shader->CompileShader({}, program, bindings, packedState.shaderHash, capture);
        stage.info = program.info;
        return stage;
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
                                                                               span<vk::DescriptorSetLayoutBinding> layoutBindings,
                                                                               const ShaderBinary &shaderBinary) {
        const bool usesPushDescriptors{ctx.gpu.traits.CanUsePushDescriptors(layoutBindings)};
        vk::raii::DescriptorSetLayout descriptorSetLayout{ctx.gpu.vkDevice, vk::DescriptorSetLayoutCreateInfo{
            .flags = vk::DescriptorSetLayoutCreateFlags{usesPushDescriptors ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR : vk::DescriptorSetLayoutCreateFlags{}},
            .bindingCount = static_cast<u32>(layoutBindings.size()),
            .pBindings = layoutBindings.data(),
        }};

        vk::raii::PipelineLayout pipelineLayout{ctx.gpu.vkDevice, vk::PipelineLayoutCreateInfo{
            .setLayoutCount = 1,
            .pSetLayouts = &*descriptorSetLayout,
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

        auto &diagnostics{ctx.gpu.shader->computeDiagnostics};
        bool captureWritten{};
        if (diagnostics.Enabled()) {
            diagnostics.Begin({}, {}, {}, false); // Invalidate the previous identity even if preparation fails.
            try {
                const auto props{ctx.gpu.vkPhysicalDevice.getProperties()};
                const auto guest{ctx.gpu.shader->GetComputeDiagnosticGuest(packedState.shaderHash, shaderBinary.binary)};
                std::string metadata{fmt::format(
                    "format=1\nstage=compute\nentry_point=main\nguest_hash={:016X}\n"
                    "guest_bytes={}\nguest_base_offset={}\nspirv_bytes={}\n"
                    "workgroup={},{},{}\nlocal_memory={}\nshared_memory={}\ntexture_cbuf={}\n"
                    "push_descriptors={}\nset_count=1\npush_constant_range_count=0\n"
                    "pipeline_flags=0\nstage_flags=0\nspecialization_info=none\n"
                    "device_name={}\napi_version={}\napplication_api_version={}\nvendor_id={}\ndevice_id={}\ndriver_version={}\n",
                    packedState.shaderHash, guest.size(), shaderBinary.baseOffset, shaderStage.diagnosticSpirvBytes,
                    packedState.dimensions[0], packedState.dimensions[1], packedState.dimensions[2],
                    packedState.localMemorySize, packedState.sharedMemorySize, packedState.bindlessTextureConstantBufferSlotSelect,
                    usesPushDescriptors, props.deviceName.data(), props.apiVersion, VkApiVersion,
                    props.vendorID, props.deviceID, props.driverVersion)};
                metadata += fmt::format("pipeline_key={:016X}/{}x{}x{}/local{}/shared{}/cbuf{}\n",
                    packedState.shaderHash, packedState.dimensions[0], packedState.dimensions[1], packedState.dimensions[2],
                    packedState.localMemorySize, packedState.sharedMemorySize, packedState.bindlessTextureConstantBufferSlotSelect);
                metadata += fmt::format("capture_time_unix_us={}\nguest_replacement={}\n",
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
                    guest.data() != shaderBinary.binary.data());
                const auto properties2{ctx.gpu.vkPhysicalDevice.getProperties2<
                    vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>()};
                const auto &driver{properties2.get<vk::PhysicalDeviceDriverProperties>()};
                metadata += fmt::format("driver_id={}\ndriver_name={}\ndriver_info={}\npipeline_cache_uuid=",
                                        vk::to_string(driver.driverID), driver.driverName.data(), driver.driverInfo.data());
                for (const auto byte : props.pipelineCacheUUID)
                    metadata += fmt::format("{:02X}", byte);
                metadata += "\n";
                for (const auto &binding : layoutBindings)
                    metadata += fmt::format("descriptor=set:0,binding:{},type:{},count:{},stage_flags:{},immutable_samplers:{}\n",
                                            binding.binding, vk::to_string(binding.descriptorType), binding.descriptorCount,
                                            static_cast<u32>(static_cast<VkShaderStageFlags>(binding.stageFlags)), binding.pImmutableSamplers != nullptr);
                metadata += ctx.gpu.shader->ComputeDiagnosticProfile();
                metadata += "traits=" + ctx.gpu.traits.Summary() + "\nquirks=" + ctx.gpu.traits.quirks.Summary() + "\n";
                const auto &spirv{shaderStage.diagnosticSpirv};
                captureWritten = diagnostics.Begin(metadata, {guest.data(), guest.size()},
                    {reinterpret_cast<const u8 *>(spirv.data()), spirv.size() * sizeof(u32)},
                    !spirv.empty() && spirv.size() * sizeof(u32) == shaderStage.diagnosticSpirvBytes);
            } catch (...) {
                // Keep diagnostic allocation/property-query failures out of Vulkan behavior.
            }
        }

        // Persist the complete latest candidate before the call that may abort in
        // a proprietary driver. No shader archive grows on successful pipelines.
        vk::raii::Pipeline pipeline{nullptr};
        try {
            pipeline = vk::raii::Pipeline{ctx.gpu.vkDevice, nullptr, pipelineInfo};
        } catch (const vk::SystemError &error) {
            if (diagnostics.Enabled()) {
                try {
                    const auto result{static_cast<vk::Result>(error.code().value())};
                    const bool resultWritten{diagnostics.Finish(error.code().value(), vk::to_string(result))};
                    AsyncLogger::LogSync(AsyncLogger::LogLevel::Error, fmt::format(
                        "ComputePipelineFailure: guest_hash={:016X} pipeline_key={:016X}/{}x{}x{}/local{}/shared{}/cbuf{} result={} ({}) capture_written={} result_written={} path={}\n{}",
                        packedState.shaderHash, packedState.shaderHash, packedState.dimensions[0], packedState.dimensions[1], packedState.dimensions[2],
                        packedState.localMemorySize, packedState.sharedMemorySize, packedState.bindlessTextureConstantBufferSlotSelect,
                        error.code().value(), vk::to_string(result), captureWritten, resultWritten,
                        diagnostics.Path().string(), error.what()));
                } catch (...) {
                    // Preserve the exact Vulkan exception if logging fails.
                }
            }
            throw;
        }
        if (diagnostics.Enabled())
            diagnostics.Finish(static_cast<i32>(vk::Result::eSuccess), "Success");

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
          compiledPipeline{MakeCompiledPipeline(ctx, packedState, shaderStage, descriptorInfo.descriptorSetLayoutBindings, shaderBinary)},
          sourcePackedState{packedState} {
        std::vector<u32>{}.swap(shaderStage.diagnosticSpirv);
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

    DescriptorUpdateInfo *Pipeline::SyncDescriptors(InterconnectContext &ctx, ConstantBufferSet &constantBuffers, Samplers &samplers, Textures &textures, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask, const std::array<u32, 3> &groups, boost::container::small_vector<TextureView *, 8> &pendingWrites) {
        SyncCachedStorageBufferViews(ctx.executor.executionTag);
        DeferredStorageImageWrites storageWriters;

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
                             auto binding{GetStorageBufferBinding(ctx, desc, constantBuffers[desc.cbuf_index],
                                                                  cachedView,
                                                                  vk::PipelineStageFlagBits::eComputeShader,
                                                                  srcStageMask, dstStageMask,
                                                                  accessSize)};
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
                                                          srcStageMask, dstStageMask, storageWriters)};
                            return binding.first;
                        });

        if (!writeIdx)
            return nullptr;

        auto *updateInfo = ctx.executor.allocator->EmplaceUntracked<DescriptorUpdateInfo>(DescriptorUpdateInfo{
            .writes = writes.first(writeIdx),
            .bufferDescs = bufferDescs.first(bufferIdx),
            .bufferDescDynamicBindings = bufferDescDynamicBindings.first(bufferIdx),
            .pipelineLayout = *compiledPipeline.pipelineLayout,
            .descriptorSetLayout = *compiledPipeline.descriptorSetLayout,
            .bindPoint = vk::PipelineBindPoint::eCompute,
            .descriptorSetIndex = 0,
        });
        pendingWrites.assign(storageWriters.begin(), storageWriters.end());
        return updateInfo;
    }
}
