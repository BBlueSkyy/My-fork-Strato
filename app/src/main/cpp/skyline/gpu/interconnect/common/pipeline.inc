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
        // SANITY CHECK: Return a dummy buffer if the view is invalid, empty, or size is 0
        if (!view || view.size == 0 || cbufSizes[idx] == 0)
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        size_t sizeOverride{std::min<size_t>(cbufSizes[idx], view.size)};
        if (sizeOverride == 0)
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        if (auto megaBufferBinding{view.TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag, sizeOverride)}) {
            return megaBufferBinding;
        } else {
            view.GetBuffer()->BlockSequencedCpuBackingWrites();
            return view;
        }
    }

    static DynamicBufferBinding GetStorageBufferBinding(InterconnectContext &ctx, const auto &desc,
                                                        ConstantBuffer &cbuf, CachedMappedBufferView &cachedView,
                                                        vk::PipelineStageFlagBits dstStage,
                                                        vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask) {
        struct SsboDescriptor {
            u64 address;
            u32 size;
        };
        auto ssbo{cbuf.Read<SsboDescriptor>(ctx.executor, desc.cbuf_offset)};

        // SANITY CHECK: Prevent invalid GPU addresses, zero size, or excessive allocation size (> 128MB)
        constexpr u64 MaxSsboSize{128 * 1024 * 1024};
        if (ssbo.address == 0 || ssbo.size == 0 || ssbo.size > MaxSsboSize)
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        size_t padding{ssbo.address & (ctx.gpu.traits.minimumStorageBufferAlignment - 1)};
        cachedView.Update(ctx, ssbo.address - padding, util::AlignUp(ssbo.size + padding, ctx.gpu.traits.minimumStorageBufferAlignment));
        if (!cachedView.view) // Return a dummy buffer if the SSBO isn't bound
            return BufferBinding{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, PAGE_SIZE).buffer, 0, PAGE_SIZE};

        auto view{cachedView.view};
        ctx.executor.AttachBuffer(view);
        view.GetBuffer()->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        if (desc.is_written) {
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

        // SAFETY CHECK: Prevents null pointer dereference (PC: 0x0 Crash)
        if (!texture || !texture->texture) {
            return {
                vk::DescriptorImageInfo{
                    .sampler = sampler ? **sampler : VK_NULL_HANDLE,
                    .imageView = VK_NULL_HANDLE,
                    .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
                },
                nullptr
            };
        }

        ctx.executor.AttachTexture(texture);
        auto view{texture->GetView()};
        texture->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        return {
            vk::DescriptorImageInfo{
                .sampler = sampler ? **sampler : VK_NULL_HANDLE,
                .imageView = view,
                .imageLayout = texture->texture->layout
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

        // SAFETY CHECK: Prevents null pointer dereference (PC: 0x0 Crash)
        if (!texture || !texture->texture) {
            return {
                vk::DescriptorImageInfo{
                    .imageView = VK_NULL_HANDLE,
                    .imageLayout = vk::ImageLayout::eGeneral
                },
                nullptr
            };
        }

        ctx.executor.AttachTexture(texture);
        texture->texture->TransitionLayout(vk::ImageLayout::eGeneral);
        texture->texture->PopulateReadBarrier(dstStage, srcStageMask, dstStageMask);

        if (desc.is_written)
            texture->texture->MarkGpuDirty(ctx.executor.usageTracker);

        return {
            vk::DescriptorImageInfo{
                .imageView = texture->GetView(),
                .imageLayout = vk::ImageLayout::eGeneral
            },
            texture
        };
    }
}
