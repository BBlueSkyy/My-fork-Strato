// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <cmath>
#include <soc/gm20b/channel.h>
#include <soc/gm20b/gmmu.h>
#include "samplers.h"
#include "dynamic_pool.h"

namespace skyline::gpu::interconnect {
    void SamplerPoolState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, texSamplerPool, texHeaderPool);
    }

    SamplerPoolState::SamplerPoolState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    void SamplerPoolState::Flush(InterconnectContext &ctx, bool useTexHeaderBinding) {
        u32 maximumIndex{useTexHeaderBinding ? engine->texHeaderPool.maximumIndex : engine->texSamplerPool.maximumIndex};
        auto mapping{ctx.channelCtx.asCtx->gmmu.LookupBlock(engine->texSamplerPool.offset)};

        texSamplers = mapping.first.subspan(mapping.second).cast<TextureSamplerControl>().first(maximumIndex + 1);

        didUseTexHeaderBinding = useTexHeaderBinding;
        ++bindingGeneration;
    }

    bool SamplerPoolState::Refresh(InterconnectContext &ctx, bool useTexHeaderBinding) {
        return didUseTexHeaderBinding != useTexHeaderBinding;
    }

    void SamplerPoolState::PurgeCaches() {
        texSamplers = span<TextureSamplerControl>{};
    }

    span<const TextureSamplerControl> SamplerPoolState::ReadDynamicHeaders(InterconnectContext &ctx) {
        const size_t count{size_t{didUseTexHeaderBinding ? engine->texHeaderPool.maximumIndex
                                                       : engine->texSamplerPool.maximumIndex} + 1};
        ReadDynamicPool(ctx, static_cast<u64>(engine->texSamplerPool.offset), count, dynamicHeaders);
        return dynamicHeaders;
    }

    Samplers::Samplers(DirtyManager &manager, const SamplerPoolState::EngineRegisters &engine) : samplerPool{manager, engine} {}

    void Samplers::Update(InterconnectContext &ctx, bool useTexHeaderBinding) {
        samplerPool.Update(ctx, useTexHeaderBinding);
        const auto *currentPool{samplerPool.Get().texSamplers.data()};
        // Index cache entries belong to the mapped pool, even when its size is
        // unchanged. Keep the content-keyed store alive for recorded commands.
        if (texSamplerCachePool != currentPool) {
            std::fill(texSamplerCache.begin(), texSamplerCache.end(), nullptr);
            texSamplerCachePool = currentPool;
        }
    }

    void Samplers::MarkAllDirty() {
        samplerPool.MarkDirty(true);
        std::fill(texSamplerCache.begin(), texSamplerCache.end(), nullptr);
    }

    span<const TextureSamplerControl> Samplers::GetPoolHeaders(InterconnectContext &ctx) {
        auto &pool{samplerPool.Get()};
        const auto headers{pool.ReadDynamicHeaders(ctx)};
        std::fill(texSamplerCache.begin(), texSamplerCache.end(), nullptr);
        return headers;
    }

    std::shared_ptr<vk::raii::Sampler> Samplers::GetDynamicSampler(InterconnectContext &ctx, u32 index) {
        const auto &headers{samplerPool.Get().dynamicHeaders};
        if (index >= headers.size()) return nullptr;
        const auto &tsc{headers[index]};
        using TSC = TextureSamplerControl;
        const auto filterValid{[](TSC::Filter filter) {
            return filter == TSC::Filter::Nearest || filter == TSC::Filter::Linear;
        }};
        const auto addressValid{[&ctx](TSC::AddressMode mode) {
            return mode == TSC::AddressMode::Repeat || mode == TSC::AddressMode::MirroredRepeat ||
                   mode == TSC::AddressMode::ClampToEdge ||
                   (mode == TSC::AddressMode::MirrorClampToEdge && ctx.gpu.traits.supportsSamplerMirrorClampToEdge);
        }};
        if (!filterValid(tsc.magFilter) || !filterValid(tsc.minFilter) ||
            (tsc.mipFilter != TSC::MipFilter::None && tsc.mipFilter != TSC::MipFilter::Nearest && tsc.mipFilter != TSC::MipFilter::Linear) ||
            !addressValid(tsc.addressModeU) || !addressValid(tsc.addressModeV) || !addressValid(tsc.addressModeP) ||
            !tsc.floatCoordNormalization || tsc.depthCompareEnable ||
            (tsc.mipFilter != TSC::MipFilter::None && tsc.minLodClamp > tsc.maxLodClamp) ||
            (tsc.maxAnisotropy && !ctx.gpu.traits.supportsAnisotropicFiltering) ||
            static_cast<u32>(tsc.reductionFilter) > static_cast<u32>(TSC::SamplerReduction::Max) ||
            (tsc.reductionFilter != TSC::SamplerReduction::WeightedAverage && !ctx.gpu.traits.supportsSamplerReductionMode))
            return nullptr;
        // The current shared sampler cache creates floating-point border colors.
        // Border samplers need numeric-class variants before they can be shared
        // with integer runtime images; reject them above instead of approximating.
        auto control{tsc};
        const auto &limits{ctx.gpu.traits.dynamicDescriptorLimits};
        if (control.MaxAnisotropy() > limits.maxSamplerAnisotropy ||
            std::abs(control.MipLodBias()) > limits.maxSamplerLodBias ||
            (tsc.reductionFilter != TSC::SamplerReduction::WeightedAverage && tsc.maxAnisotropy))
            return nullptr;
        if (index < texSamplerCache.size()) texSamplerCache[index] = nullptr;
        auto &pool{samplerPool.Get()};
        struct RestoreHeaders {
            SamplerPoolState &pool;
            span<TextureSamplerControl> previous;
            std::vector<vk::raii::Sampler *> &cache;
            u32 index;
            ~RestoreHeaders() {
                pool.texSamplers = previous;
                if (index < cache.size()) cache[index] = nullptr;
            }
        } restore{pool, pool.texSamplers, texSamplerCache, index};
        pool.texSamplers = pool.dynamicHeaders;
        GetSampler(ctx, index);
        return texSamplerStore.at(tsc);
    }

    bool Samplers::DynamicSamplerUsesLinear(u32 index) {
        const auto &tsc{samplerPool.Get().dynamicHeaders.at(index)};
        return tsc.magFilter == TextureSamplerControl::Filter::Linear ||
               tsc.minFilter == TextureSamplerControl::Filter::Linear ||
               tsc.mipFilter == TextureSamplerControl::MipFilter::Linear || tsc.maxAnisotropy;
    }

    static vk::Filter ConvertSamplerFilter(TextureSamplerControl::Filter filter) {
        switch (filter) {
            case TextureSamplerControl::Filter::Nearest:
                return vk::Filter::eNearest;
            case TextureSamplerControl::Filter::Linear:
                return vk::Filter::eLinear;
        }
        // Reserved descriptor values must not cause an undefined C++ return.
        // Match the deterministic nearest fallback used by the reference backends.
        LOGW("Invalid sampler filter {}", static_cast<u32>(filter));
        return vk::Filter::eNearest;
    }

    static vk::SamplerMipmapMode ConvertSamplerMipFilter(TextureSamplerControl::MipFilter filter) {
        switch (filter) {
            // See https://github.com/yuzu-emu/yuzu/blob/5af06d14337a61d9ed1093079d13f68cbb1f5451/src/video_core/renderer_vulkan/maxwell_to_vk.cpp#L35
            case TextureSamplerControl::MipFilter::None:
                return vk::SamplerMipmapMode::eNearest;
            case TextureSamplerControl::MipFilter::Nearest:
                return vk::SamplerMipmapMode::eNearest;
            case TextureSamplerControl::MipFilter::Linear:
                return vk::SamplerMipmapMode::eLinear;
        }
        LOGW("Invalid sampler mip filter {}", static_cast<u32>(filter));
        return vk::SamplerMipmapMode::eNearest;
    }

    static vk::SamplerAddressMode ConvertSamplerAddressMode(TextureSamplerControl::AddressMode mode) {
        switch (mode) {
            case TextureSamplerControl::AddressMode::Repeat:
                return vk::SamplerAddressMode::eRepeat;
            case TextureSamplerControl::AddressMode::MirroredRepeat:
                return vk::SamplerAddressMode::eMirroredRepeat;

            case TextureSamplerControl::AddressMode::ClampToEdge:
                return vk::SamplerAddressMode::eClampToEdge;
            case TextureSamplerControl::AddressMode::ClampToBorder:
                return vk::SamplerAddressMode::eClampToBorder;
            case TextureSamplerControl::AddressMode::Clamp:
                return vk::SamplerAddressMode::eClampToEdge; // Vulkan doesn't support 'GL_CLAMP' so this is an approximation

            case TextureSamplerControl::AddressMode::MirrorClampToEdge:
                return vk::SamplerAddressMode::eMirrorClampToEdge;
            case TextureSamplerControl::AddressMode::MirrorClampToBorder:
                return vk::SamplerAddressMode::eMirrorClampToEdge; // Only supported mirror clamps are to edges so this is an approximation
            case TextureSamplerControl::AddressMode::MirrorClamp:
                return vk::SamplerAddressMode::eMirrorClampToEdge; // Same as above
        }
    }

    static vk::CompareOp ConvertSamplerCompareOp(TextureSamplerControl::CompareOp compareOp) {
        switch (compareOp) {
            case TextureSamplerControl::CompareOp::Never:
                return vk::CompareOp::eNever;
            case TextureSamplerControl::CompareOp::Less:
                return vk::CompareOp::eLess;
            case TextureSamplerControl::CompareOp::Equal:
                return vk::CompareOp::eEqual;
            case TextureSamplerControl::CompareOp::LessOrEqual:
                return vk::CompareOp::eLessOrEqual;
            case TextureSamplerControl::CompareOp::Greater:
                return vk::CompareOp::eGreater;
            case TextureSamplerControl::CompareOp::NotEqual:
                return vk::CompareOp::eNotEqual;
            case TextureSamplerControl::CompareOp::GreaterOrEqual:
                return vk::CompareOp::eGreaterOrEqual;
            case TextureSamplerControl::CompareOp::Always:
                return vk::CompareOp::eAlways;
        }
    }

    static Shader::CompareFunction ConvertSamplerCompareFunc(TextureSamplerControl::CompareOp compareOp) {
        switch (compareOp) {
            case TextureSamplerControl::CompareOp::Never:
                return Shader::CompareFunction::Never;
            case TextureSamplerControl::CompareOp::Less:
                return Shader::CompareFunction::Less;
            case TextureSamplerControl::CompareOp::Equal:
                return Shader::CompareFunction::Equal;
            case TextureSamplerControl::CompareOp::LessOrEqual:
                return Shader::CompareFunction::LessThanEqual;
            case TextureSamplerControl::CompareOp::Greater:
                return Shader::CompareFunction::Greater;
            case TextureSamplerControl::CompareOp::NotEqual:
                return Shader::CompareFunction::NotEqual;
            case TextureSamplerControl::CompareOp::GreaterOrEqual:
                return Shader::CompareFunction::GreaterThanEqual;
            case TextureSamplerControl::CompareOp::Always:
                return Shader::CompareFunction::Always;
         }
     }
   
     static vk::SamplerReductionMode ConvertSamplerReductionFilter(TextureSamplerControl::SamplerReduction reduction) {
        switch (reduction) {
            case TextureSamplerControl::SamplerReduction::WeightedAverage:
                return vk::SamplerReductionMode::eWeightedAverage;
            case TextureSamplerControl::SamplerReduction::Min:
                return vk::SamplerReductionMode::eMin;
            case TextureSamplerControl::SamplerReduction::Max:
                return vk::SamplerReductionMode::eMax;
        }
    }

    static vk::BorderColor ConvertBorderColorWithCustom(float red, float green, float blue, float alpha) {
        if (alpha == 1.0f) {
            if (red == 1.0f && green == 1.0f && blue == 1.0f)
                return vk::BorderColor::eFloatOpaqueWhite;
            else if (red == 0.0f && green == 0.0f && blue == 0.0f)
                return vk::BorderColor::eFloatOpaqueBlack;
        } else if (red == 1.0f && green == 1.0f && blue == 1.0f && alpha == 0.0f) {
            return vk::BorderColor::eFloatTransparentBlack;
        }

        return vk::BorderColor::eFloatCustomEXT;
    }

    static vk::BorderColor ConvertBorderColorFixed(float red, float green, float blue, float alpha) {
        if (alpha == 1.0f) {
            if (red == 1.0f && green == 1.0f && blue == 1.0f)
                return vk::BorderColor::eFloatOpaqueWhite;
            else if (red == 0.0f && green == 0.0f && blue == 0.0f)
                return vk::BorderColor::eFloatOpaqueBlack;
        } else if (red == 1.0f && green == 1.0f && blue == 1.0f && alpha == 0.0f) {
            return vk::BorderColor::eFloatTransparentBlack;
        }

        // Approximations of a custom color using fixed colors
        if (red + green + blue > 1.0f)
            return vk::BorderColor::eFloatOpaqueWhite;
        else if (alpha > 0.0f)
            return vk::BorderColor::eFloatOpaqueBlack;
        else
            return vk::BorderColor::eFloatTransparentBlack;
    }

    vk::raii::Sampler *Samplers::GetSampler(InterconnectContext &ctx, u32 samplerIndex, u32 textureIndex) {
        const auto &samplerPoolObj{samplerPool.Get()};
        return GetSampler(ctx, samplerPoolObj.didUseTexHeaderBinding ? textureIndex : samplerIndex);
    }

    vk::raii::Sampler *Samplers::GetSampler(InterconnectContext &ctx, u32 index) {
        const auto &samplerPoolObj{samplerPool.Get()};
        auto texSamplers{samplerPoolObj.texSamplers};
        if (index >= texSamplers.size()) {
            LOGW("Sampler requested for out-of-range TSC index {}", index);
            return nullptr;
        }

        if (texSamplers.size() != texSamplerCache.size()) {
            texSamplerCache.resize(texSamplers.size());
            std::fill(texSamplerCache.begin(), texSamplerCache.end(), nullptr);
        } else if (texSamplerCache[index]) {
            return texSamplerCache[index];
        }

        TextureSamplerControl &texSampler{texSamplers[index]};
        auto &sampler{texSamplerStore[texSampler]};
        if (!sampler) {
            auto convertAddressModeWithCheck{[&](TextureSamplerControl::AddressMode mode) {
                auto vkMode{ConvertSamplerAddressMode(mode)};
                if (vkMode == vk::SamplerAddressMode::eMirrorClampToEdge && !ctx.gpu.traits.supportsSamplerMirrorClampToEdge) [[unlikely]] {
                    LOGW("Cannot use Mirror Clamp To Edge as Sampler Address Mode without host GPU support");
                    return vk::SamplerAddressMode::eClampToEdge; // We use a normal clamp to edge to approximate it
                }
                return vkMode;
            }};

            auto maxAnisotropy{texSampler.MaxAnisotropy()};
            vk::StructureChain<vk::SamplerCreateInfo, vk::SamplerReductionModeCreateInfoEXT, vk::SamplerCustomBorderColorCreateInfoEXT> samplerInfo{
                vk::SamplerCreateInfo{
                    .magFilter = ConvertSamplerFilter(texSampler.magFilter),
                    .minFilter = ConvertSamplerFilter(texSampler.minFilter),
                    .mipmapMode = ConvertSamplerMipFilter(texSampler.mipFilter),
                    .addressModeU = convertAddressModeWithCheck(texSampler.addressModeU),
                    .addressModeV = convertAddressModeWithCheck(texSampler.addressModeV),
                    .addressModeW = convertAddressModeWithCheck(texSampler.addressModeP),
                    .mipLodBias = texSampler.MipLodBias(),
                    .anisotropyEnable = ctx.gpu.traits.supportsAnisotropicFiltering && maxAnisotropy > 1.0f,
                    .maxAnisotropy = maxAnisotropy,
                    .compareEnable = !ctx.gpu.traits.quirks.brokenTextureShadowCompare && texSampler.depthCompareEnable,
                    .compareOp = ConvertSamplerCompareOp(texSampler.depthCompareOp),
                    .minLod = texSampler.mipFilter == TextureSamplerControl::MipFilter::None ? 0.0f : texSampler.MinLodClamp(),
                    .maxLod = texSampler.mipFilter == TextureSamplerControl::MipFilter::None ? 0.25f : texSampler.MaxLodClamp(),
                    .unnormalizedCoordinates = false,
                }, vk::SamplerReductionModeCreateInfoEXT{
                    .reductionMode = ConvertSamplerReductionFilter(texSampler.reductionFilter),
                }, vk::SamplerCustomBorderColorCreateInfoEXT{
                    .customBorderColor.float32 = {{texSampler.borderColorR, texSampler.borderColorG, texSampler.borderColorB, texSampler.borderColorA}},
                    .format = vk::Format::eUndefined,
                },
            };

            if (!ctx.gpu.traits.supportsSamplerReductionMode)
                samplerInfo.unlink<vk::SamplerReductionModeCreateInfoEXT>();

            vk::BorderColor &borderColor{samplerInfo.get<vk::SamplerCreateInfo>().borderColor};
            if (ctx.gpu.traits.supportsCustomBorderColor) {
                borderColor = ConvertBorderColorWithCustom(texSampler.borderColorR, texSampler.borderColorG, texSampler.borderColorB, texSampler.borderColorA);
                if (borderColor != vk::BorderColor::eFloatCustomEXT)
                    samplerInfo.unlink<vk::SamplerCustomBorderColorCreateInfoEXT>();
            } else {
                borderColor = ConvertBorderColorFixed(texSampler.borderColorR, texSampler.borderColorG, texSampler.borderColorB, texSampler.borderColorA);
                samplerInfo.unlink<vk::SamplerCustomBorderColorCreateInfoEXT>();
            }

            sampler = std::make_shared<vk::raii::Sampler>(ctx.gpu.vkDevice, samplerInfo.get<vk::SamplerCreateInfo>());
        }

        texSamplerCache[index] = sampler.get();
        return sampler.get();
    }

    Shader::CompareFunction Samplers::GetTextureCompareFunc(InterconnectContext &ctx, u32 samplerIndex, u32 textureIndex) {
        const auto &samplerPoolObj{samplerPool.Get()};
        u32 index{samplerPoolObj.didUseTexHeaderBinding ? textureIndex : samplerIndex};
        if (index >= samplerPoolObj.texSamplers.size()) {
            LOGW("Shadow comparison requested for out-of-range TSC index {}", index);
            return Shader::CompareFunction::LessThanEqual;
        }

        const TextureSamplerControl &texSampler{samplerPoolObj.texSamplers[index]};
        return ConvertSamplerCompareFunc(texSampler.depthCompareOp);
    }
}
