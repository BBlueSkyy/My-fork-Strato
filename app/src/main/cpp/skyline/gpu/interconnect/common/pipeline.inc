// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <gpu/interconnect/command_executor.h>
#include "samplers.h"
#include "textures.h"

namespace skyline::gpu::interconnect {
    union BindlessHandle {
        u32 raw;

        struct {
            u32 textureIndex : 20;
            u32 samplerIndex : 12;
        };
    };

    static DynamicBufferBinding GetConstantBufferBinding(InterconnectContext &ctx,
                                                         span<const u32, Shader::Info::MAX_CBUFS> cbufSizes,
                                                         BufferView view, size_t idx,
                                                         vk::PipelineStageFlagBits dstStage,
                                                         vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        if (!view) // Retorna um buffer dummy se o constant buffer não estiver vinculado
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        size_t sizeOverride{std::min<size_t>(cbufSizes[idx], view.size)};
        if (auto megaBufferBinding{view.TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag, sizeOverride)}) {
            return megaBufferBinding;
        } else {
            view.GetBuffer()->BlockSequencedCpuBackingWrites();
            return view;
        }
    }

    struct StorageBufferGuestDescriptor {
        u64 address;
        u32 size;
    };

    static DynamicBufferBinding GetStorageBufferBinding(InterconnectContext &ctx, const auto &desc,
                                                        ConstantBuffer &cbuf, CachedMappedBufferView &cachedView,
                                                        vk::PipelineStageFlagBits dstStage,
                                                        vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask,
                                                        std::optional<u64> accessSize = std::nullopt) {
        const StorageBufferGuestDescriptor ssbo{desc.has_size
            ? cbuf.Read<StorageBufferGuestDescriptor>(ctx.executor, desc.cbuf_offset)
            : StorageBufferGuestDescriptor{cbuf.Read<u64>(ctx.executor, desc.cbuf_offset), 0}};
        if (ssbo.address == 0 || (desc.has_size && ssbo.size == 0))
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        size_t padding{ssbo.address & (ctx.gpu.traits.minimumStorageBufferAlignment - 1)};
        constexpr u64 MaximumUnknownStorageBufferRange{8 * 1024 * 1024};
        const u64 pointerRange{accessSize ? util::AlignUp(*accessSize + padding, ctx.gpu.traits.minimumStorageBufferAlignment)
                                         : MaximumUnknownStorageBufferRange};
        const u64 range{desc.has_size ? std::min<u64>(util::AlignUp(u64{ssbo.size} + padding, ctx.gpu.traits.minimumStorageBufferAlignment), ctx.gpu.traits.maximumStorageBufferRange)
                                     : std::min<u64>(pointerRange, ctx.gpu.traits.maximumStorageBufferRange)};
        if (!range)
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};
        cachedView.Update(ctx, ssbo.address - padding, range, desc.has_size);
        if (!cachedView.view) // Retorna um buffer dummy se o SSBO não estiver vinculado
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        auto view{cachedView.view};
        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        if (desc.is_written) {
            // Barreiras de memória para escritas em SSBO (sincronização de etapas de Vertex/Compute Shader)
            srcStageMask |= vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eTransfer;
            dstStageMask |= dstStage | vk::PipelineStageFlagBits::eVertexInput | vk::PipelineStageFlagBits::eVertexShader;

            if (view.GetBuffer()->SequencedCpuBackingWritesBlocked()) {
                srcStageMask |= vk::PipelineStageFlagBits::eAllCommands;
                dstStageMask |= dstStage;
            }

            view.GetBuffer()->MarkGpuDirty(ctx.executor.usageTracker);
        } else {
            if (auto megaBufferBinding{view.TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag)})
                return megaBufferBinding;
        }

        view.GetBuffer()->BlockSequencedCpuBackingWrites();

        return view;
    }

    static vk::BufferView GetTextureBufferBinding(InterconnectContext &ctx,
                                                   Textures &textures, BindlessHandle handle,
                                                   CachedMappedBufferView &cachedView,
                                                   vk::PipelineStageFlagBits dstStage,
                                                   vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto *bufferView{textures.GetTextureBufferView(ctx, handle.textureIndex, cachedView)};
        if (!bufferView)
            return {};

        auto view{cachedView.view};
        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);
        view.GetBuffer()->BlockSequencedCpuBackingWrites();

        return **bufferView;
    }

    static vk::BufferView GetImageBufferBinding(InterconnectContext &ctx,
                                                 Textures &textures, BindlessHandle handle,
                                                 Shader::ImageFormat format, bool isWritten,
                                                 CachedMappedBufferView &cachedView,
                                                 vk::PipelineStageFlagBits dstStage,
                                                 vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto *bufferView{textures.GetImageBufferView(ctx, handle.textureIndex, format, cachedView)};
        if (!bufferView)
            return {};

        auto view{cachedView.view};
        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        if (isWritten) {
            if (view.GetBuffer()->SequencedCpuBackingWritesBlocked()) {
                srcStageMask |= vk::PipelineStageFlagBits::eAllCommands;
                dstStageMask |= dstStage;
            }
            view.GetBuffer()->MarkGpuDirty(ctx.executor.usageTracker);
        }

        view.GetBuffer()->BlockSequencedCpuBackingWrites();

        return **bufferView;
    }

    static BindlessHandle ReadBindlessHandle(InterconnectContext &ctx, auto &constantBuffers, const auto &desc, size_t arrayIdx) {
        ConstantBuffer &primaryCbuf{constantBuffers[desc.cbuf_index]};
        if (!primaryCbuf.view)
            return { .raw = 0 };

        size_t elemOffset{arrayIdx << desc.size_shift};
        size_t primaryCbufOffset{desc.cbuf_offset + elemOffset};
        u32 primaryVal{primaryCbuf.Read<u32>(ctx.executor, primaryCbufOffset)};

        if constexpr (requires { desc.has_secondary; }) {
            if (desc.has_secondary) {
                ConstantBuffer &secondaryCbuf{constantBuffers[desc.secondary_cbuf_index]};
                size_t secondaryCbufOffset{desc.secondary_cbuf_offset + elemOffset};
                u32 secondaryVal{secondaryCbuf.Read<u32>(ctx.executor, secondaryCbufOffset)};
                return {.raw = (primaryVal << desc.shift_left) | (secondaryVal << desc.secondary_shift_left)};
            }
        }

        return {.raw = primaryVal};
    }

    static std::pair<vk::DescriptorImageInfo, TextureView *> GetTextureBinding(InterconnectContext &ctx, const auto &desc,
                                                                               Samplers &samplers, Textures &textures,
                                                                               BindlessHandle handle,
                                                                               vk::PipelineStageFlagBits dstStage,
                                                                               vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto sampler{samplers.GetSampler(ctx, handle.samplerIndex, handle.textureIndex)};
        auto texture{textures.GetTexture(ctx, handle.textureIndex, desc.type)};

        if (!texture) {
            return {
                vk::DescriptorImageInfo{},
                nullptr
            };
        }

        ctx.executor.AttachTexture(texture);
        auto view{texture->GetView()};

        if (texture->texture) {
            texture->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);
        }

        vk::Sampler vkSampler = sampler ? **sampler : VK_NULL_HANDLE;
        vk::ImageLayout layout = texture->texture ? texture->texture->layout : vk::ImageLayout::eShaderReadOnlyOptimal;

        return {
            vk::DescriptorImageInfo{
                .sampler = vkSampler,
                .imageView = view,
                .imageLayout = layout
            },
            texture
        };
    }

    static std::pair<vk::DescriptorImageInfo, TextureView *> GetImageBinding(InterconnectContext &ctx, const auto &desc,
                                                                              Textures &textures,
                                                                              BindlessHandle handle,
                                                                              vk::PipelineStageFlagBits dstStage,
                                                                              vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        auto texture{textures.GetTexture(ctx, handle.textureIndex, desc.type)};

        if (!texture) {
            return {
                vk::DescriptorImageInfo{},
                nullptr
            };
        }

        ctx.executor.AttachTexture(texture);
        if (!(texture->GetUsage() & vk::ImageUsageFlagBits::eStorage))
            throw exception("Texture view format does not support storage image binding");
        // Storage images access raw components; sampled TIC swizzles are invalid here.
        auto view{texture->GetView(true)};

        if (texture->texture) {
            texture->texture->TransitionLayout(vk::ImageLayout::eGeneral);
            texture->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

            if (desc.is_written)
                texture->texture->MarkGpuDirty(ctx.executor.usageTracker);
        }

        return {
            vk::DescriptorImageInfo{
                .imageView = view,
                .imageLayout = vk::ImageLayout::eGeneral
            },
            texture
        };
    }
}
