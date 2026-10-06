// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <gpu/descriptor_allocator.h>
#include <gpu/graphics_pipeline_assembler.h>

namespace skyline::vfs {
    class FileSystem;
}

namespace skyline::gpu {
    class TextureView;
    class GPU;

    /**
     * @brief A base class that can be inherited by helper shaders that render to a single color/depth rendertarget to simplify pipeline creation
     */
    class SimpleSingleRtShader {
      protected:
        /**
         * @brief Holds all per-pipeline state for a helper shader
         */
        struct PipelineState {
            vk::Format colorFormat;
            vk::Format depthFormat;
            u32 stencilValue;
            VkColorComponentFlags colorWriteMask;
            bool depthWrite;
            bool stencilWrite;

            bool operator==(const PipelineState &input) const {
                return std::memcmp(this, &input, sizeof(PipelineState)) == 0;
            }
        };

        std::unordered_map<PipelineState, GraphicsPipelineAssembler::CompiledPipeline, util::ObjectHash<PipelineState>> pipelineCache;
        vk::raii::ShaderModule vertexShaderModule;
        vk::raii::ShaderModule fragmentShaderModule;

        std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages; //!< Shader stages for the vertex and fragment shader modules

        SimpleSingleRtShader(GPU &gpu, std::shared_ptr<vfs::Backing> vertexShader, std::shared_ptr<vfs::Backing> fragmentShader);

        /**
         * @brief Returns a potentially cached pipeline built according to the supplied input state
         */
        const GraphicsPipelineAssembler::CompiledPipeline &GetPipeline(GPU &gpu,
                                                                            const PipelineState &state,
                                                                            span<const vk::DescriptorSetLayoutBinding> layoutBindings, span<const vk::PushConstantRange> pushConstantRanges);
    };

    /**
     * @brief Simple helper shader for blitting a texture to a rendertarget with subpixel-precision
     */
    class BlitHelperShader : SimpleSingleRtShader {
      private:
        vk::raii::Sampler bilinearSampler;
        vk::raii::Sampler nearestSampler;

      public:
            BlitHelperShader(GPU &gpu, std::shared_ptr<vfs::FileSystem> shaderFileSystem);

            /**
             * @brief Floating point equivalent to vk::Rect2D to allow for subpixel-precison blits
             */
            struct BlitRect {
                float width;
                float height;
                float x;
                float y;
            };

            /**
             * @brief Records a sequenced GPU blit operation
             * @param srcRect A subrect of the source input texture that will be blitted from
             * @param dstRect A subrect of the destination input texture that the source subrect will be blitted into
             * @param dstSrcScaleFactorX Scale factor in the X direction from the destination image to the source image
             * @param dstSrcScaleFactorY ^ but Y
             * @param bilinearFilter Type of filter to use for sampling the source texture, false will use nearest-neighbour and true will use bilinear filtering
             * @param recordCb Callback used to record the blit commands for sequenced execution on the GPU
             */
            void Blit(GPU &gpu, BlitRect srcRect, BlitRect dstRect,
                      vk::Extent2D srcImageDimensions, vk::Extent2D dstImageDimensions,
                      float dstSrcScaleFactorX, float dstSrcScaleFactorY,
                      bool bilinearFilter,
                      TextureView *srcImageView, TextureView *dstImageView,
                      std::function<void(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32)> &&)> &&recordCb);
    };

    /**
     * @brief Simple helper shader for clearing a texture to a given color
     */
    class ClearHelperShader : SimpleSingleRtShader {
      public:
        ClearHelperShader(GPU &gpu, std::shared_ptr<vfs::FileSystem> shaderFileSystem);

        /**
         * @brief Records a sequenced GPU clear operation using a shader
         * @param mask Mask of which aspects to clear
         * @param components Mask of which components to clear
         * @param value The value to clear to
         * @param recordCb Callback used to record the blit commands for sequenced execution on the GPU
         */
        void Clear(GPU &gpu, vk::ImageAspectFlags mask, vk::ColorComponentFlags components, vk::ClearValue value, TextureView *dstImageView,
                  std::function<void(std::function<void(vk::raii::CommandBuffer &, const std::shared_ptr<FenceCycle> &, GPU &, vk::RenderPass, u32)> &&)> &&recordCb);
    };

    /**
     * @brief Accumulates 64-bit occlusion-query segments on the GPU without a CPU readback
     */
    class QueryResolveHelperShader {
      private:
        vk::raii::ShaderModule shaderModule;
        vk::raii::DescriptorSetLayout descriptorSetLayout;
        vk::raii::PipelineLayout pipelineLayout;
        vk::raii::Pipeline pipeline;

      public:
        struct PushConstants {
            u32 firstIndex;
            u32 count;
            u32 resetAccumulator;
        };

        QueryResolveHelperShader(GPU &gpu, std::shared_ptr<vfs::FileSystem> shaderFileSystem);

        DescriptorAllocator::ActiveDescriptorSet CreateDescriptorSet(GPU &gpu, vk::Buffer queryResults, vk::DeviceSize queryResultsSize, vk::Buffer accumulator);

        void Resolve(vk::raii::CommandBuffer &commandBuffer, vk::DescriptorSet descriptorSet, u32 firstIndex, u32 count, bool resetAccumulator);

        void PrepareForTransfer(vk::raii::CommandBuffer &commandBuffer);
    };

    /**
     * @brief Produces a 32-bit conditional-rendering predicate from two GPU-visible u32 values
     */
    class ConditionalCompareHelperShader {
      private:
        vk::raii::ShaderModule shaderModule;
        vk::raii::DescriptorSetLayout descriptorSetLayout;
        vk::raii::PipelineLayout pipelineLayout;
        vk::raii::Pipeline pipeline;

      public:
        struct PushConstants {
            u32 notEqual;
        };

        ConditionalCompareHelperShader(GPU &gpu, std::shared_ptr<vfs::FileSystem> shaderFileSystem);

        DescriptorAllocator::ActiveDescriptorSet CreateDescriptorSet(GPU &gpu, vk::Buffer scratchBuffer);

        void Compare(vk::raii::CommandBuffer &commandBuffer, vk::DescriptorSet descriptorSet, bool notEqual);
    };

    /**
     * @brief Holds all helper shaders to avoid redundantly recreating them on each usage
     */
    struct HelperShaders {
        BlitHelperShader blitHelperShader;
        ClearHelperShader clearHelperShader;
        QueryResolveHelperShader queryResolveHelperShader;
        ConditionalCompareHelperShader conditionalCompareHelperShader;

        HelperShaders(GPU &gpu, std::shared_ptr<vfs::FileSystem> shaderFileSystem);
    };


}
