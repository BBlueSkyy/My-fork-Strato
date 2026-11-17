// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Ryujinx Team and Contributors (https://github.com/ryujinx/)
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu/buffer_manager.h>
#include <soc/gm20b/gmmu.h>
#include <soc/gm20b/channel.h>
#include "inline2memory.h"

namespace skyline::gpu::interconnect {
    using IOVA = soc::gm20b::IOVA;

    Inline2Memory::Inline2Memory(GPU &gpu, soc::gm20b::ChannelContext &channelCtx)
        : gpu{gpu},
          channelCtx{channelCtx},
          executor{channelCtx.executor} {}

    void Inline2Memory::Upload(IOVA dst, span<u8> src) {
        executor.RunWithTextureBackingRetry([&] {
            auto &gmmu{channelCtx.asCtx->gmmu};
            const auto epoch{gmmu.ReadMappingEpoch()};
            executor.StartTextureMapping(epoch);
            const auto mappings{gmmu.TranslateRange(dst, src.size())};
            boost::container::small_vector<BufferView, 4> views;
            for (auto mapping : mappings)
                views.push_back(gpu.buffer.FindOrCreate(mapping, executor.tag, [this](std::shared_ptr<Buffer> buffer, ContextLock<Buffer> &&lock) {
                    executor.AttachLockedBuffer(buffer, std::move(lock));
                }));
            // All lookups finish before resolving delegates: a later lookup may
            // coalesce an earlier view into a different Buffer.
            if (views.size() == 1) {
                auto buffer{views.front().GetBuffer()->shared_from_this()};
                buffer->RequireRegionCoherence(executor.tag);
                ContextLock lock{executor.tag, *buffer};
                if (buffer->TryMappedWrite(src, views.front().GetOffset(), executor.usageTracker, epoch)) return;
            }
            for (auto &view : views) {
                executor.AttachBuffer(view);
                view.GetBuffer()->PrepareGpuWrite(executor.usageTracker);
            }
            auto allocation{gpu.megaBufferAllocator.Push(executor.cycle, src)};
            // One node and one publication lease cover the complete upload.
            executor.AddOutsideRpCommand([views, allocation](vk::raii::CommandBuffer &commands, const std::shared_ptr<FenceCycle> &, GPU &gpu) {
                vk::DeviceSize offset{};
                for (auto view : views) {
                    commands.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, vk::MemoryBarrier{
                        .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferWrite
                    }, {}, {});
                    auto binding{view.GetBinding(gpu)};
                    commands.copyBuffer(allocation.buffer, binding.buffer, vk::BufferCopy{
                        .srcOffset = allocation.offset + offset, .dstOffset = binding.offset, .size = view.size});
                    offset += view.size;
                }
                commands.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, vk::MemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                }, {}, {});
            }, {}, span<BufferView>{views.data(), views.size()});
        });
    }
}
